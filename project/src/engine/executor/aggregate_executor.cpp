// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include "datatype_document.hpp"
#include "datatype_operations.hpp"
#include "temp_spill_executor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
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

bool DescriptorBatchCarrierIsExactDefault(const DescriptorBatch& batch) {
  const DescriptorBatch empty;
  return batch.columns.empty() &&
         batch.columns.capacity() == empty.columns.capacity() &&
         batch.rows.empty() && batch.rows.capacity() == empty.rows.capacity();
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

struct AggregateTransitionValuesView {
  std::array<const EngineTypedValue*, 2> values{};
  std::size_t size = 0;

  const EngineTypedValue& operator[](const std::size_t index) const {
    return *values[index];
  }

  const EngineTypedValue& front() const { return *values.front(); }
  bool empty() const { return size == 0; }
};

bool AggregateDistinctKey(const AggregateTransitionValuesView& values,
                          std::string* key,
                          DescriptorRuntimeDiagnostic* diagnostic);
bool AggregateDistinctKeyAllocationBound(
    const AggregateTransitionValuesView& values,
    std::size_t* bytes);
bool CheckedAggregateFinalizationAdd(std::size_t* total,
                                     std::size_t amount);
bool CheckedAggregateFinalizationMultiply(std::size_t left,
                                          std::size_t right,
                                          std::size_t* product);

bool WellFormedAggregateUtf8(const std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::uint32_t code_point = 0;
    std::size_t continuation_count = 0;
    if (first <= 0x7f) {
      code_point = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1f;
      continuation_count = 1;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0f;
      continuation_count = 2;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07;
      continuation_count = 3;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1) return false;
    for (std::size_t index = 1; index <= continuation_count; ++index) {
      const auto next = static_cast<unsigned char>(value[offset + index]);
      if ((next & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    offset += continuation_count + 1;
  }
  return true;
}

bool CanonicalizeAggregateJson(const std::string& input,
                               std::string* canonical) {
  if (canonical == nullptr || !WellFormedAggregateUtf8(input)) return false;
  scratchbird::core::datatypes::DocumentCanonicalizationRequest request;
  request.type_id =
      scratchbird::core::datatypes::CanonicalTypeId::json_document;
  request.encoded_value = input;
  const auto result =
      scratchbird::core::datatypes::CanonicalizeDocumentValue(request);
  if (!result.ok() || result.canonical_type_id != request.type_id ||
      !WellFormedAggregateUtf8(result.canonical_value)) {
    return false;
  }
  *canonical = result.canonical_value;
  return true;
}

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

void AppendAggregateJsonEscaped(const std::string_view input,
                                std::string* output) {
  if (output == nullptr) return;
  output->push_back('"');
  static constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char byte : input) {
    switch (byte) {
      case '\\': output->append("\\\\"); break;
      case '"': output->append("\\\""); break;
      case '\b': output->append("\\b"); break;
      case '\f': output->append("\\f"); break;
      case '\n': output->append("\\n"); break;
      case '\r': output->append("\\r"); break;
      case '\t': output->append("\\t"); break;
      default:
        if (byte < 0x20) {
          output->append("\\u00");
          output->push_back(kHex[(byte >> 4) & 0x0f]);
          output->push_back(kHex[byte & 0x0f]);
        } else {
          output->push_back(static_cast<char>(byte));
        }
        break;
    }
  }
  output->push_back('"');
}

bool RenderAggregateScalarPayload(const EngineTypedValue& value,
                                  std::string* rendered) {
  if (rendered == nullptr) return false;
  const auto& type = value.descriptor.canonical_type_name;
  const bool binary_backed =
      type == "binary" || type == "blob" || type == "bytes" ||
      (value.encoded_value.empty() && !value.binary_value.empty());
  if (!binary_backed || value.binary_value.empty()) {
    *rendered = value.encoded_value;
    return true;
  }
  if (value.binary_value.size() >
      std::numeric_limits<std::size_t>::max() / 2) {
    return false;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  rendered->clear();
  rendered->reserve(value.binary_value.size() * 2);
  for (const auto byte : value.binary_value) {
    rendered->push_back(kHex[(byte >> 4) & 0x0f]);
    rendered->push_back(kHex[byte & 0x0f]);
  }
  return true;
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
  const auto type_id =
      scratchbird::core::datatypes::CanonicalTypeIdFromStableName(type);
  if (type_id == scratchbird::core::datatypes::CanonicalTypeId::int8 ||
      type_id == scratchbird::core::datatypes::CanonicalTypeId::int16 ||
      type_id == scratchbird::core::datatypes::CanonicalTypeId::int32 ||
      type_id == scratchbird::core::datatypes::CanonicalTypeId::int64) {
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
  if (type == "json" ||
      type_id == scratchbird::core::datatypes::CanonicalTypeId::json_document ||
      type_id == scratchbird::core::datatypes::CanonicalTypeId::object_document ||
      type_id == scratchbird::core::datatypes::CanonicalTypeId::flattened_object_document) {
    if (!CanonicalizeAggregateJson(value.encoded_value, rendered)) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-JSON-V1",
                            "JSON aggregate input is not canonical JSON");
      return false;
    }
    return true;
  }
  std::string scalar;
  if (!RenderAggregateScalarPayload(value, &scalar)) {
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                          "aggregate scalar rendering overflowed");
    return false;
  }
  if (!WellFormedAggregateUtf8(scalar)) {
    *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-JSON-V1",
                          "JSON aggregate scalar is not valid UTF-8");
    return false;
  }
  *rendered = EscapeAggregateJson(scalar);
  return true;
}

std::size_t EstimateCanonicalAggregateStateBytes(
    const CanonicalAggregateCoreState& state) {
  const auto add = [](std::size_t* total, const std::size_t amount) {
    if (*total > std::numeric_limits<std::size_t>::max() - amount) {
      *total = std::numeric_limits<std::size_t>::max();
      return;
    }
    *total += amount;
  };
  std::size_t bytes = sizeof(state);
  const auto add_value_dynamic_bytes = [&](const EngineTypedValue& value) {
    add(&bytes, value.descriptor.descriptor_uuid.canonical.size());
    add(&bytes, value.descriptor.descriptor_kind.size());
    add(&bytes, value.encoded_value.size());
    add(&bytes, value.binary_value.size());
    add(&bytes, value.descriptor.canonical_type_name.size());
    add(&bytes, value.descriptor.encoded_descriptor.size());
  };
  if (state.extremum.has_value()) {
    add_value_dynamic_bytes(*state.extremum);
  }
  if (state.collection_values.capacity() >
      std::numeric_limits<std::size_t>::max() / sizeof(EngineTypedValue)) {
    bytes = std::numeric_limits<std::size_t>::max();
  } else {
    add(&bytes, state.collection_values.capacity() *
                    sizeof(EngineTypedValue));
  }
  for (const auto& value : state.collection_values) {
    add_value_dynamic_bytes(value);
  }
  if (state.text_values.capacity() >
      std::numeric_limits<std::size_t>::max() / sizeof(std::string)) {
    bytes = std::numeric_limits<std::size_t>::max();
  } else {
    add(&bytes, state.text_values.capacity() * sizeof(std::string));
  }
  for (const auto& value : state.text_values) {
    add(&bytes, value.size());
  }
  if (state.json_object_values.capacity() >
      std::numeric_limits<std::size_t>::max() /
          sizeof(std::pair<std::string, std::string>)) {
    bytes = std::numeric_limits<std::size_t>::max();
  } else {
    add(&bytes, state.json_object_values.capacity() *
                    sizeof(std::pair<std::string, std::string>));
  }
  for (const auto& [key, value] : state.json_object_values) {
    add(&bytes, key.size());
    add(&bytes, value.size());
  }
  if (state.ordered_numeric_values.capacity() >
      std::numeric_limits<std::size_t>::max() / sizeof(long double)) {
    bytes = std::numeric_limits<std::size_t>::max();
  } else {
    add(&bytes,
        state.ordered_numeric_values.capacity() * sizeof(long double));
  }
  if (state.approximate_distinct_values.capacity() >
      std::numeric_limits<std::size_t>::max() / sizeof(std::string)) {
    bytes = std::numeric_limits<std::size_t>::max();
  } else {
    add(&bytes, state.approximate_distinct_values.capacity() *
                    sizeof(std::string));
  }
  for (const auto& value : state.approximate_distinct_values) {
    add(&bytes, value.size());
  }
  if (state.frequency_values.capacity() >
      std::numeric_limits<std::size_t>::max() /
          sizeof(std::pair<EngineTypedValue, std::size_t>)) {
    bytes = std::numeric_limits<std::size_t>::max();
  } else {
    add(&bytes, state.frequency_values.capacity() *
                    sizeof(std::pair<EngineTypedValue, std::size_t>));
  }
  for (const auto& [value, count] : state.frequency_values) {
    (void)count;
    add_value_dynamic_bytes(value);
  }
  return bytes;
}

bool ReserveCanonicalAggregateCoreState(
    const CanonicalAggregateDescriptor& descriptor,
    const std::size_t transition_capacity,
    const std::size_t maximum_state_bytes,
    CanonicalAggregateCoreState* state,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (state == nullptr || diagnostic == nullptr) return false;
  std::size_t element_bytes = 0;
  if (descriptor.function == CanonicalAggregateFunction::array_agg) {
    element_bytes = sizeof(EngineTypedValue);
  } else if (descriptor.function == CanonicalAggregateFunction::json_agg ||
             descriptor.function == CanonicalAggregateFunction::string_agg ||
             descriptor.function == CanonicalAggregateFunction::listagg ||
             descriptor.function ==
                 CanonicalAggregateFunction::approx_count_distinct) {
    element_bytes = sizeof(std::string);
  } else if (descriptor.function ==
             CanonicalAggregateFunction::json_object_agg) {
    element_bytes = sizeof(std::pair<std::string, std::string>);
  } else if (IsCanonicalHypotheticalSetFunction(descriptor.function) ||
             IsCanonicalQuantileFunction(descriptor.function)) {
    element_bytes = sizeof(long double);
  } else if (descriptor.function == CanonicalAggregateFunction::mode ||
             descriptor.function == CanonicalAggregateFunction::approx_top_k) {
    element_bytes = sizeof(std::pair<EngineTypedValue, std::size_t>);
  }
  std::size_t structural_bytes = 0;
  const auto current_bytes = EstimateCanonicalAggregateStateBytes(*state);
  if (element_bytes != 0 &&
      !CheckedAggregateFinalizationMultiply(transition_capacity,
                                             element_bytes,
                                             &structural_bytes)) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate state capacity size overflowed before allocation");
    return false;
  }
  if (current_bytes > maximum_state_bytes ||
      structural_bytes > maximum_state_bytes - current_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate state capacity exceeds the selected-node grant");
    return false;
  }
  try {
    if (descriptor.function == CanonicalAggregateFunction::array_agg) {
      state->collection_values.reserve(transition_capacity);
    } else if (descriptor.function == CanonicalAggregateFunction::json_agg ||
               descriptor.function ==
                   CanonicalAggregateFunction::string_agg ||
               descriptor.function == CanonicalAggregateFunction::listagg) {
      state->text_values.reserve(transition_capacity);
    } else if (descriptor.function ==
               CanonicalAggregateFunction::json_object_agg) {
      state->json_object_values.reserve(transition_capacity);
    } else if (IsCanonicalHypotheticalSetFunction(descriptor.function) ||
               IsCanonicalQuantileFunction(descriptor.function)) {
      state->ordered_numeric_values.reserve(transition_capacity);
    } else if (descriptor.function ==
               CanonicalAggregateFunction::approx_count_distinct) {
      state->approximate_distinct_values.reserve(transition_capacity);
    } else if (descriptor.function == CanonicalAggregateFunction::mode ||
               descriptor.function ==
                   CanonicalAggregateFunction::approx_top_k) {
      state->frequency_values.reserve(transition_capacity);
    }
  } catch (const std::bad_alloc&) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate state capacity allocation was refused");
    return false;
  } catch (const std::length_error&) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate state capacity allocation exceeded the container limit");
    return false;
  }
  if (EstimateCanonicalAggregateStateBytes(*state) > maximum_state_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate state capacity exceeds the selected-node grant");
    return false;
  }
  return true;
}

bool AggregateBatchPayloadBytes(const DescriptorBatch& batch,
                                std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (*bytes > std::numeric_limits<std::size_t>::max() -
                       value.encoded_value.size()) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (*bytes > std::numeric_limits<std::size_t>::max() -
                       value.binary_value.size()) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool AggregateValueStateBytes(const EngineTypedValue& value,
                              std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = sizeof(value);
  return CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.descriptor_uuid.canonical.size()) &&
         CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.descriptor_kind.size()) &&
         CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.canonical_type_name.size()) &&
         CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.encoded_descriptor.size()) &&
         CheckedAggregateFinalizationAdd(bytes,
                                          value.encoded_value.size()) &&
         CheckedAggregateFinalizationAdd(bytes,
                                          value.binary_value.size());
}

bool AggregateValueDynamicStateBytes(const EngineTypedValue& value,
                                     std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 0;
  return CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.descriptor_uuid.canonical.size()) &&
         CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.descriptor_kind.size()) &&
         CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.canonical_type_name.size()) &&
         CheckedAggregateFinalizationAdd(
             bytes, value.descriptor.encoded_descriptor.size()) &&
         CheckedAggregateFinalizationAdd(bytes,
                                          value.encoded_value.size()) &&
         CheckedAggregateFinalizationAdd(bytes,
                                          value.binary_value.size());
}

bool AggregateTransitionAllocationBound(
    const CanonicalAggregateDescriptor& descriptor,
    const AggregateTransitionValuesView& values,
    std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 0;
  if (values.empty() ||
      (descriptor.function == CanonicalAggregateFunction::count &&
       descriptor.count_star)) {
    return true;
  }
  const auto add_value = [&](const EngineTypedValue& value) {
    std::size_t value_bytes = 0;
    return AggregateValueDynamicStateBytes(value, &value_bytes) &&
           CheckedAggregateFinalizationAdd(bytes, value_bytes);
  };
  const auto& value = values.front();
  if (descriptor.function == CanonicalAggregateFunction::array_agg ||
      descriptor.function == CanonicalAggregateFunction::min ||
      descriptor.function == CanonicalAggregateFunction::max ||
      descriptor.function == CanonicalAggregateFunction::mode ||
      descriptor.function == CanonicalAggregateFunction::approx_top_k) {
    return add_value(value);
  }
  if (descriptor.function == CanonicalAggregateFunction::string_agg ||
      descriptor.function == CanonicalAggregateFunction::listagg) {
    *bytes = value.encoded_value.size();
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::json_agg ||
      descriptor.function == CanonicalAggregateFunction::json_object_agg) {
    std::size_t payload_bytes = 0;
    for (std::size_t index = 0; index < values.size; ++index) {
      if (!CheckedAggregateFinalizationAdd(
              &payload_bytes, values[index].encoded_value.size()) ||
          !CheckedAggregateFinalizationAdd(
              &payload_bytes, values[index].binary_value.size())) {
        return false;
      }
    }
    if (!CheckedAggregateFinalizationMultiply(payload_bytes, 12,
                                                &payload_bytes)) {
      return false;
    }
    *bytes = payload_bytes;
    return CheckedAggregateFinalizationAdd(bytes, 16);
  }
  if (IsCanonicalHypotheticalSetFunction(descriptor.function) ||
      IsCanonicalQuantileFunction(descriptor.function)) {
    *bytes = 0;
    return true;
  }
  if (descriptor.function ==
      CanonicalAggregateFunction::approx_count_distinct) {
    std::size_t key_bytes = 0;
    if (!AggregateDistinctKeyAllocationBound(values, &key_bytes)) {
      return false;
    }
    *bytes = key_bytes;
    return true;
  }
  return true;
}

std::optional<std::size_t> AggregateNodeMemoryGrant(
    const TypedPhysicalNodeDag& dag, const PhysicalNodeRecord& node) {
  if (dag.memory_budget_bytes == 0 || node.memory_bytes_required == 0 ||
      node.memory_bytes_required > dag.memory_budget_bytes ||
      node.memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(node.memory_bytes_required);
}

bool AggregateTextValuesEncodedSize(
    const std::vector<std::string>& values, std::size_t count,
    std::string_view separator, std::size_t* encoded_size);

std::string JoinAggregateTextValues(const std::vector<std::string>& values,
                                    const std::size_t count,
                                    const std::string_view separator) {
  std::string joined;
  const auto limit = std::min(count, values.size());
  std::size_t encoded_size = 0;
  if (!AggregateTextValuesEncodedSize(values, limit, separator,
                                      &encoded_size)) {
    throw std::length_error("aggregate text size overflow");
  }
  joined.reserve(encoded_size);
  for (std::size_t index = 0; index < limit; ++index) {
    if (index != 0) joined += separator;
    joined += values[index];
  }
  return joined;
}

bool AggregateTextValuesEncodedSize(
    const std::vector<std::string>& values, const std::size_t count,
    const std::string_view separator, std::size_t* encoded_size) {
  if (encoded_size == nullptr) return false;
  *encoded_size = 0;
  const auto limit = std::min(count, values.size());
  for (std::size_t index = 0; index < limit; ++index) {
    const auto separator_size = index == 0 ? 0 : separator.size();
    if (*encoded_size > std::numeric_limits<std::size_t>::max() -
                            separator_size) {
      return false;
    }
    *encoded_size += separator_size;
    if (*encoded_size >
        std::numeric_limits<std::size_t>::max() - values[index].size()) {
      return false;
    }
    *encoded_size += values[index].size();
  }
  return true;
}

bool SameAggregateValueIdentity(const EngineTypedValue& left,
                                const EngineTypedValue& right) {
  return left.descriptor.descriptor_uuid.canonical ==
             right.descriptor.descriptor_uuid.canonical &&
         left.descriptor.descriptor_kind ==
             right.descriptor.descriptor_kind &&
         left.descriptor.canonical_type_name ==
             right.descriptor.canonical_type_name &&
         left.descriptor.encoded_descriptor ==
             right.descriptor.encoded_descriptor &&
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
  value.is_null = false;
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
  if (value == 0.0L) return "0";
  std::array<char, 128> buffer{};
  const auto rendered = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(),
      static_cast<double>(value), std::chars_format::general, 17);
  if (rendered.ec != std::errc{}) return {};
  return std::string(buffer.data(), rendered.ptr);
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
    const AggregateTransitionValuesView& values,
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
  if (values.size != expected_arity) {
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
      if (!WellFormedAggregateUtf8(key_text)) {
        *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-JSON-KEY-V1",
                              "JSON object aggregate key is not valid UTF-8");
        return false;
      }
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
    std::string identity;
    AggregateTransitionValuesView identity_value;
    identity_value.values[0] = &value;
    identity_value.size = 1;
    if (!AggregateDistinctKey(identity_value, &identity, diagnostic)) {
      return false;
    }
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

bool TransitionCanonicalAggregateCoreBounded(
    const CanonicalAggregateDescriptor& descriptor,
    const AggregateTransitionValuesView& values,
    const std::size_t maximum_live_state_bytes,
    CanonicalAggregateCoreState* state,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (state == nullptr || diagnostic == nullptr) return false;
  const auto current_state_bytes =
      EstimateCanonicalAggregateStateBytes(*state);
  std::size_t allocation_bound = 0;
  if (current_state_bytes > maximum_live_state_bytes ||
      !AggregateTransitionAllocationBound(descriptor, values,
                                          &allocation_bound) ||
      allocation_bound >
          maximum_live_state_bytes - current_state_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate transition allocation exceeds the selected-node grant");
    return false;
  }
  if (!TransitionCanonicalAggregateCore(descriptor, values, state,
                                        diagnostic)) {
    return false;
  }
  if (EstimateCanonicalAggregateStateBytes(*state) >
      maximum_live_state_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate transition state exceeds the selected-node grant");
    return false;
  }
  return true;
}

bool InverseCanonicalAggregateCore(
    const CanonicalAggregateDescriptor& descriptor,
    const AggregateTransitionValuesView& values,
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
  if (values.size != 1) {
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
  std::int64_t decoded_integer = 0;
  long double decoded_numeric = 0.0L;
  bool signed_integer = false;
  if (descriptor.function == CanonicalAggregateFunction::sum ||
      descriptor.function == CanonicalAggregateFunction::avg) {
    if (IsCanonicalBoundedSignedIntegerDescriptor(value.descriptor)) {
      const auto decoded = DecodeInt64Value(value);
      if (!decoded.ok()) {
        *diagnostic = decoded.diagnostic;
        return false;
      }
      decoded_integer = decoded.value;
      decoded_numeric = static_cast<long double>(decoded.value);
      signed_integer = true;
    } else if (IsType(value, "real64")) {
      const auto decoded = DecodeReal64Value(value);
      if (!decoded.ok()) {
        *diagnostic = decoded.diagnostic;
        return false;
      }
      decoded_numeric = static_cast<long double>(decoded.value);
    } else {
      *diagnostic = Refusal(
          "QOW-DIAG-QRY-011-REGISTRY-INVERSE-TYPE-V1",
          "moving SUM and AVG inverse state requires bounded signed integer "
          "or real64 input");
      return false;
    }
  }
  --state->transition_count;
  --state->non_null_count;
  if (descriptor.function == CanonicalAggregateFunction::sum ||
      descriptor.function == CanonicalAggregateFunction::avg) {
    if (signed_integer) {
      state->int64_sum -= static_cast<__int128>(decoded_integer);
    }
    state->real_sum -= decoded_numeric;
    if (!std::isfinite(state->real_sum)) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
                            "floating inverse aggregate state is not finite");
      return false;
    }
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
    CanonicalAggregateCoreState&& source,
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
        std::make_move_iterator(source.collection_values.begin()),
        std::make_move_iterator(source.collection_values.end()));
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::string_agg ||
      descriptor.function == CanonicalAggregateFunction::listagg ||
      descriptor.function == CanonicalAggregateFunction::json_agg) {
    target->non_null_count += source.non_null_count;
    target->text_values.insert(target->text_values.end(),
        std::make_move_iterator(source.text_values.begin()),
        std::make_move_iterator(source.text_values.end()));
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::json_object_agg) {
    target->non_null_count += source.non_null_count;
    for (auto& member : source.json_object_values) {
      const auto existing = std::find_if(
          target->json_object_values.begin(), target->json_object_values.end(),
          [&](const auto& candidate) {
            return candidate.first == member.first;
          });
      if (existing != target->json_object_values.end()) {
        target->json_object_values.erase(existing);
      }
      target->json_object_values.push_back(std::move(member));
    }
    return true;
  }
  if (IsCanonicalHypotheticalSetFunction(descriptor.function) ||
      IsCanonicalQuantileFunction(descriptor.function)) {
    target->non_null_count += source.non_null_count;
    target->ordered_numeric_values.insert(
        target->ordered_numeric_values.end(),
        std::make_move_iterator(source.ordered_numeric_values.begin()),
        std::make_move_iterator(source.ordered_numeric_values.end()));
    return true;
  }
  if (descriptor.function == CanonicalAggregateFunction::mode ||
      descriptor.function == CanonicalAggregateFunction::approx_top_k) {
    target->non_null_count += source.non_null_count;
    for (auto& frequency : source.frequency_values) {
      const auto existing = std::find_if(
          target->frequency_values.begin(), target->frequency_values.end(),
          [&](const auto& candidate) {
            return SameAggregateValueIdentity(candidate.first,
                                              frequency.first);
          });
      if (existing == target->frequency_values.end()) {
        target->frequency_values.push_back(std::move(frequency));
      } else {
        existing->second += frequency.second;
      }
    }
    return true;
  }
  if (descriptor.function ==
      CanonicalAggregateFunction::approx_count_distinct) {
    target->non_null_count += source.non_null_count;
    for (auto& identity : source.approximate_distinct_values) {
      if (std::find(target->approximate_distinct_values.begin(),
                    target->approximate_distinct_values.end(), identity) ==
          target->approximate_distinct_values.end()) {
        target->approximate_distinct_values.push_back(std::move(identity));
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
    target->extremum = std::move(source.extremum);
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
    target->extremum = std::move(source.extremum);
  }
  return true;
}

bool MergeCanonicalAggregateCoreBounded(
    const CanonicalAggregateDescriptor& descriptor,
    CanonicalAggregateCoreState* target,
    CanonicalAggregateCoreState&& source,
    const std::size_t maximum_live_state_bytes,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (target == nullptr || diagnostic == nullptr) return false;
  const auto target_bytes = EstimateCanonicalAggregateStateBytes(*target);
  const auto source_bytes = EstimateCanonicalAggregateStateBytes(source);
  const auto merge_capacity_available = [&]() {
    const auto function = descriptor.function;
    if (function == CanonicalAggregateFunction::array_agg) {
      return source.collection_values.size() <=
             target->collection_values.capacity() -
                 target->collection_values.size();
    }
    if (function == CanonicalAggregateFunction::string_agg ||
        function == CanonicalAggregateFunction::listagg ||
        function == CanonicalAggregateFunction::json_agg) {
      return source.text_values.size() <=
             target->text_values.capacity() - target->text_values.size();
    }
    if (function == CanonicalAggregateFunction::json_object_agg) {
      return source.json_object_values.size() <=
             target->json_object_values.capacity() -
                 target->json_object_values.size();
    }
    if (IsCanonicalHypotheticalSetFunction(function) ||
        IsCanonicalQuantileFunction(function)) {
      return source.ordered_numeric_values.size() <=
             target->ordered_numeric_values.capacity() -
                 target->ordered_numeric_values.size();
    }
    if (function == CanonicalAggregateFunction::mode ||
        function == CanonicalAggregateFunction::approx_top_k) {
      return source.frequency_values.size() <=
             target->frequency_values.capacity() -
                 target->frequency_values.size();
    }
    if (function == CanonicalAggregateFunction::approx_count_distinct) {
      return source.approximate_distinct_values.size() <=
             target->approximate_distinct_values.capacity() -
                 target->approximate_distinct_values.size();
    }
    return true;
  };
  if (target_bytes > maximum_live_state_bytes ||
      source_bytes > maximum_live_state_bytes - target_bytes ||
      !merge_capacity_available()) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate merge allocation exceeds the selected-node grant");
    return false;
  }
  if (!MergeCanonicalAggregateCore(descriptor, target, std::move(source),
                                   diagnostic)) {
    return false;
  }
  if (EstimateCanonicalAggregateStateBytes(*target) >
      maximum_live_state_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate merged state exceeds the selected-node grant");
    return false;
  }
  return true;
}

bool ValidateCanonicalAggregateResultType(
    const CanonicalAggregateRuntimeRequest& request,
    const DescriptorBatch& input_batch,
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
        input_batch.columns[request.value_columns.front()];
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
  } else if ((function == CanonicalAggregateFunction::min ||
              function == CanonicalAggregateFunction::max) &&
             !request.value_columns.empty()) {
    const auto& input =
        input_batch.columns[request.value_columns.front()];
    if (request.result_column.nullable &&
        ((IsCanonicalBoundedSignedIntegerDescriptor(input.descriptor) &&
          IsType(request.result_column, "int64")) ||
         request.result_column.descriptor.canonical_type_name ==
             input.descriptor.canonical_type_name)) {
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
  } else if (function == CanonicalAggregateFunction::mode &&
             !request.value_columns.empty()) {
    const auto& input =
        input_batch.columns[request.value_columns.front()];
    if (request.result_column.nullable &&
        ((IsCanonicalBoundedSignedIntegerDescriptor(input.descriptor) &&
          IsType(request.result_column, "int64")) ||
         request.result_column.descriptor.canonical_type_name ==
             input.descriptor.canonical_type_name)) {
      return true;
    }
  } else if (!request.value_columns.empty()) {
    const auto& input = input_batch.columns[request.value_columns[0]];
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
    const DescriptorBatch& input_batch,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (request.descriptor.count_star ||
      request.descriptor.function == CanonicalAggregateFunction::count) {
    return true;
  }
  if (IsCanonicalPairStatisticalFunction(request.descriptor.function)) {
    for (const auto column : request.value_columns) {
      const auto& descriptor =
          input_batch.columns[column].descriptor;
      if (!IsCanonicalBoundedSignedIntegerDescriptor(descriptor) &&
          descriptor.canonical_type_name != "real64") {
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
    const auto& key_type = input_batch
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
    const auto& type = input_batch
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
    const auto& type = input_batch
                           .columns[request.value_columns.front()]
                           .descriptor.canonical_type_name;
    if (IsCanonicalBoundedSignedIntegerDescriptor(
            input_batch
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
          CanonicalAggregateFunction::approx_count_distinct) {
    return true;
  }
  if (request.descriptor.function ==
      CanonicalAggregateFunction::approx_top_k) {
    const auto& type =
        input_batch.columns[request.value_columns.front()]
            .descriptor.canonical_type_name;
    if (type == "text" || type == "varchar" || type == "char") {
      return true;
    }
    *diagnostic = Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-TYPE-V1",
        "APPROX_TOP_K requires a canonical text input descriptor");
    return false;
  }
  const auto& type = input_batch
                         .columns[request.value_columns.front()]
                         .descriptor.canonical_type_name;
  if (request.descriptor.function == CanonicalAggregateFunction::sum ||
      request.descriptor.function == CanonicalAggregateFunction::avg ||
      IsCanonicalUnivariateStatisticalFunction(
          request.descriptor.function)) {
    if (IsCanonicalBoundedSignedIntegerDescriptor(
            input_batch
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

struct CanonicalAggregateFinalizationPlan {
  std::size_t output_bytes = 0;
  std::size_t peak_workspace_bytes = 0;
  std::vector<std::size_t> top_k_order;
};

struct CanonicalAggregateFinalizationReceipt {
  std::size_t output_bytes = 0;
  std::size_t peak_workspace_bytes = 0;
};

bool CheckedAggregateFinalizationAdd(std::size_t* total,
                                     const std::size_t amount) {
  if (total == nullptr ||
      *total > std::numeric_limits<std::size_t>::max() - amount) {
    return false;
  }
  *total += amount;
  return true;
}

bool CheckedAggregateFinalizationMultiply(const std::size_t left,
                                          const std::size_t right,
                                          std::size_t* product) {
  if (product == nullptr ||
      (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)) {
    return false;
  }
  *product = left * right;
  return true;
}

std::size_t AggregateDecimalDigitCount(std::size_t value) {
  std::size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

bool AggregateScalarRenderedSize(const EngineTypedValue& value,
                                 std::size_t* size) {
  if (size == nullptr) return false;
  const auto& type = value.descriptor.canonical_type_name;
  const bool binary_backed =
      type == "binary" || type == "blob" || type == "bytes" ||
      (value.encoded_value.empty() && !value.binary_value.empty());
  if (!binary_backed || value.binary_value.empty()) {
    *size = value.encoded_value.size();
    return true;
  }
  return CheckedAggregateFinalizationMultiply(value.binary_value.size(), 2,
                                                size);
}

bool AggregateJsonEscapedSize(const std::string_view value,
                              std::size_t* size) {
  if (size == nullptr) return false;
  *size = 2;
  for (const unsigned char byte : value) {
    const auto encoded =
        byte < 0x20 ? 6U
                    : (byte == '\\' || byte == '"' ? 2U : 1U);
    if (!CheckedAggregateFinalizationAdd(size, encoded)) return false;
  }
  return true;
}

bool AggregateScalarJsonEscapedSize(const EngineTypedValue& value,
                                    std::size_t* scalar_bytes,
                                    std::size_t* escaped_bytes) {
  if (scalar_bytes == nullptr || escaped_bytes == nullptr) return false;
  const auto& type = value.descriptor.canonical_type_name;
  const bool binary_backed =
      type == "binary" || type == "blob" || type == "bytes" ||
      (value.encoded_value.empty() && !value.binary_value.empty());
  if (binary_backed && !value.binary_value.empty()) {
    if (!CheckedAggregateFinalizationMultiply(value.binary_value.size(), 2,
                                                scalar_bytes)) {
      return false;
    }
    *escaped_bytes = 2;
    return CheckedAggregateFinalizationAdd(escaped_bytes, *scalar_bytes);
  }
  *scalar_bytes = value.encoded_value.size();
  return AggregateJsonEscapedSize(value.encoded_value, escaped_bytes);
}

bool AggregateTypedValuePayloadBytes(const EngineTypedValue& value,
                                     std::size_t* size) {
  if (size == nullptr) return false;
  *size = value.encoded_value.size();
  return CheckedAggregateFinalizationAdd(size, value.binary_value.size());
}

bool AggregateFrequencyIdentityLess(const EngineTypedValue& left,
                                    const EngineTypedValue& right) {
  const auto left_state = static_cast<std::underlying_type_t<EngineValueState>>(
      left.state);
  const auto right_state =
      static_cast<std::underlying_type_t<EngineValueState>>(right.state);
  return std::tie(left.descriptor.descriptor_uuid.canonical,
                  left.descriptor.descriptor_kind,
                  left.descriptor.canonical_type_name,
                  left.descriptor.encoded_descriptor, left_state,
                  left.is_null, left.encoded_value, left.binary_value) <
         std::tie(right.descriptor.descriptor_uuid.canonical,
                  right.descriptor.descriptor_kind,
                  right.descriptor.canonical_type_name,
                  right.descriptor.encoded_descriptor, right_state,
                  right.is_null, right.encoded_value, right.binary_value);
}

bool PlanCanonicalAggregateFinalization(
    const CanonicalAggregateRuntimeRequest& request,
    const CanonicalAggregateCoreState& state,
    CanonicalAggregateFinalizationPlan* plan,
    DescriptorRuntimeDiagnostic* diagnostic,
    const std::size_t maximum_finalization_workspace_bytes,
    const std::size_t maximum_live_finalization_bytes) {
  if (plan == nullptr || diagnostic == nullptr) return false;
  *plan = {};
  const auto refuse_overflow = [&]() {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate finalization size overflowed before allocation");
    return false;
  };
  const auto function = request.descriptor.function;
  const bool collection_function =
      function == CanonicalAggregateFunction::array_agg ||
      function == CanonicalAggregateFunction::json_agg ||
      function == CanonicalAggregateFunction::json_object_agg ||
      function == CanonicalAggregateFunction::string_agg ||
      function == CanonicalAggregateFunction::listagg ||
      function == CanonicalAggregateFunction::mode ||
      function == CanonicalAggregateFunction::approx_top_k;
  if ((!collection_function &&
       function != CanonicalAggregateFunction::count &&
       function != CanonicalAggregateFunction::regr_count &&
       function != CanonicalAggregateFunction::approx_count_distinct &&
       !IsCanonicalHypotheticalSetFunction(function) &&
       state.non_null_count == 0) ||
      ((function == CanonicalAggregateFunction::array_agg ||
        function == CanonicalAggregateFunction::json_agg ||
        function == CanonicalAggregateFunction::json_object_agg) &&
       state.transition_count == 0) ||
      ((function == CanonicalAggregateFunction::string_agg ||
        function == CanonicalAggregateFunction::listagg) &&
       state.text_values.empty()) ||
      ((function == CanonicalAggregateFunction::mode ||
        function == CanonicalAggregateFunction::approx_top_k) &&
       state.frequency_values.empty())) {
    return true;
  }
  if (function == CanonicalAggregateFunction::count ||
      function == CanonicalAggregateFunction::regr_count) {
    plan->output_bytes =
        AggregateDecimalDigitCount(state.non_null_count);
    return true;
  }
  if (function == CanonicalAggregateFunction::approx_count_distinct) {
    plan->output_bytes = AggregateDecimalDigitCount(
        state.approximate_distinct_values.size());
    return true;
  }
  if (IsCanonicalHypotheticalSetFunction(function) &&
      state.ordered_numeric_values.empty()) {
    plan->output_bytes = 1;
    return true;
  }
  if (function == CanonicalAggregateFunction::array_agg) {
    plan->output_bytes = 6;
    for (std::size_t index = 0; index < state.collection_values.size();
         ++index) {
      if (index != 0 &&
          !CheckedAggregateFinalizationAdd(&plan->output_bytes, 1)) {
        return refuse_overflow();
      }
      const auto& value = state.collection_values[index];
      if (value.state == EngineValueState::sql_null) {
        if (!CheckedAggregateFinalizationAdd(&plan->output_bytes, 4)) {
          return refuse_overflow();
        }
        continue;
      }
      std::size_t scalar_bytes = 0;
      if (!AggregateScalarRenderedSize(value, &scalar_bytes) ||
          !CheckedAggregateFinalizationAdd(
              &plan->output_bytes,
              value.descriptor.canonical_type_name.size()) ||
          !CheckedAggregateFinalizationAdd(&plan->output_bytes, 1) ||
          !CheckedAggregateFinalizationAdd(&plan->output_bytes,
                                            scalar_bytes)) {
        return refuse_overflow();
      }
      plan->peak_workspace_bytes =
          std::max(plan->peak_workspace_bytes, scalar_bytes);
    }
    return true;
  }
  if (function == CanonicalAggregateFunction::json_agg) {
    plan->output_bytes = 2;
    for (std::size_t index = 0; index < state.text_values.size(); ++index) {
      if ((index != 0 &&
           !CheckedAggregateFinalizationAdd(&plan->output_bytes, 1)) ||
          !CheckedAggregateFinalizationAdd(&plan->output_bytes,
                                            state.text_values[index].size())) {
        return refuse_overflow();
      }
    }
    return true;
  }
  if (function == CanonicalAggregateFunction::json_object_agg) {
    plan->output_bytes = 2;
    for (std::size_t index = 0; index < state.json_object_values.size();
         ++index) {
      std::size_t key_bytes = 0;
      if (!AggregateJsonEscapedSize(state.json_object_values[index].first,
                                    &key_bytes) ||
          (index != 0 &&
           !CheckedAggregateFinalizationAdd(&plan->output_bytes, 1)) ||
          !CheckedAggregateFinalizationAdd(&plan->output_bytes, key_bytes) ||
          !CheckedAggregateFinalizationAdd(&plan->output_bytes, 1) ||
          !CheckedAggregateFinalizationAdd(
              &plan->output_bytes,
              state.json_object_values[index].second.size())) {
        return refuse_overflow();
      }
    }
    return true;
  }
  if (function == CanonicalAggregateFunction::string_agg ||
      function == CanonicalAggregateFunction::listagg) {
    std::size_t full_bytes = 0;
    if (!AggregateTextValuesEncodedSize(
            state.text_values, state.text_values.size(),
            request.aggregate_separator, &full_bytes)) {
      return refuse_overflow();
    }
    plan->output_bytes = full_bytes;
    if (function == CanonicalAggregateFunction::listagg &&
        request.listagg_overflow_mode != CanonicalListaggOverflowMode::none &&
        full_bytes > request.listagg_max_output_bytes) {
      plan->output_bytes =
          request.listagg_overflow_mode == CanonicalListaggOverflowMode::error
              ? 0
              : request.listagg_max_output_bytes;
      const auto indicator_bytes =
          request.listagg_truncation_indicator.empty()
              ? 3
              : request.listagg_truncation_indicator.size();
      plan->peak_workspace_bytes = indicator_bytes;
      if (request.listagg_with_count &&
          !CheckedAggregateFinalizationAdd(&plan->peak_workspace_bytes, 22)) {
        return refuse_overflow();
      }
    }
    return true;
  }
  if (IsCanonicalHypotheticalSetFunction(function) ||
      IsCanonicalQuantileFunction(function)) {
    plan->output_bytes = 64;
    if (!CheckedAggregateFinalizationMultiply(
            state.ordered_numeric_values.size(), sizeof(long double),
            &plan->peak_workspace_bytes)) {
      return refuse_overflow();
    }
    return true;
  }
  if (function == CanonicalAggregateFunction::mode) {
    for (const auto& candidate : state.frequency_values) {
      std::size_t candidate_bytes = 0;
      if (!AggregateTypedValuePayloadBytes(candidate.first,
                                           &candidate_bytes)) {
        return refuse_overflow();
      }
      plan->output_bytes = std::max(plan->output_bytes, candidate_bytes);
    }
    return true;
  }
  if (function == CanonicalAggregateFunction::min ||
      function == CanonicalAggregateFunction::max) {
    return !state.extremum.has_value() ||
           AggregateTypedValuePayloadBytes(*state.extremum,
                                           &plan->output_bytes);
  }
  if (function == CanonicalAggregateFunction::approx_top_k) {
    plan->output_bytes = 2;
    std::size_t order_bytes = 0;
    if (!CheckedAggregateFinalizationMultiply(state.frequency_values.size(),
                                                sizeof(std::size_t),
                                                &order_bytes)) {
      return refuse_overflow();
    }
    std::size_t ordering_peak_workspace_bytes = order_bytes;
    if (ordering_peak_workspace_bytes >
            maximum_finalization_workspace_bytes ||
        ordering_peak_workspace_bytes > maximum_live_finalization_bytes) {
      *diagnostic = Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "APPROX_TOP_K ordering workspace exceeds its selected-node grant");
      return false;
    }
    plan->top_k_order.resize(state.frequency_values.size());
    std::iota(plan->top_k_order.begin(), plan->top_k_order.end(), 0);
    std::sort(plan->top_k_order.begin(), plan->top_k_order.end(),
              [&](const auto left, const auto right) {
                if (state.frequency_values[left].second !=
                    state.frequency_values[right].second) {
                  return state.frequency_values[left].second >
                         state.frequency_values[right].second;
                }
                return AggregateFrequencyIdentityLess(
                    state.frequency_values[left].first,
                    state.frequency_values[right].first);
              });
    const auto decoded = DecodeInt64Value(request.direct_arguments.front());
    if (!decoded.ok()) {
      *diagnostic = decoded.diagnostic;
      return false;
    }
    const auto limit = std::min(static_cast<std::size_t>(decoded.value),
                                state.frequency_values.size());
    std::size_t maximum_scalar_bytes = 0;
    std::size_t maximum_count_bytes = 0;
    for (std::size_t rank = 0; rank < limit; ++rank) {
      const auto& frequency =
          state.frequency_values[plan->top_k_order[rank]];
      std::size_t scalar_bytes = 0;
      std::size_t escaped_bytes = 0;
      const auto& value = frequency.first;
      if (!AggregateScalarJsonEscapedSize(value, &scalar_bytes,
                                          &escaped_bytes)) {
        return refuse_overflow();
      }
      std::size_t entry_bytes = 19;
      if (!CheckedAggregateFinalizationAdd(&entry_bytes, escaped_bytes) ||
          !CheckedAggregateFinalizationAdd(
              &entry_bytes,
              AggregateDecimalDigitCount(frequency.second)) ||
          !CheckedAggregateFinalizationAdd(&plan->output_bytes,
                                            entry_bytes) ||
          (rank != 0 &&
           !CheckedAggregateFinalizationAdd(&plan->output_bytes, 1))) {
        return refuse_overflow();
      }
      maximum_scalar_bytes = std::max(maximum_scalar_bytes, scalar_bytes);
      maximum_count_bytes = std::max(
          maximum_count_bytes,
          AggregateDecimalDigitCount(frequency.second));
    }
    std::size_t rendering_peak_workspace_bytes = order_bytes;
    if (!CheckedAggregateFinalizationAdd(&rendering_peak_workspace_bytes,
                                          maximum_scalar_bytes) ||
        !CheckedAggregateFinalizationAdd(&rendering_peak_workspace_bytes,
                                          maximum_count_bytes)) {
      return refuse_overflow();
    }
    plan->peak_workspace_bytes = std::max(
        ordering_peak_workspace_bytes, rendering_peak_workspace_bytes);
    return true;
  }
  plan->output_bytes =
      function == CanonicalAggregateFunction::bool_and ||
              function == CanonicalAggregateFunction::bool_or ||
              function == CanonicalAggregateFunction::every
          ? 5
          : 64;
  return true;
}

EngineTypedValue FinalizeCanonicalAggregateCoreUnchecked(
    const CanonicalAggregateRuntimeRequest& request,
    const CanonicalAggregateCoreState& state,
    DescriptorRuntimeDiagnostic* diagnostic,
    const CanonicalAggregateFinalizationPlan& plan) {
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
    encoded.reserve(plan.output_bytes);
    for (std::size_t index = 0; index < state.collection_values.size();
         ++index) {
      if (index != 0) encoded.push_back(';');
      const auto& value = state.collection_values[index];
      if (value.state == EngineValueState::sql_null) {
        encoded += "NULL";
      } else {
        encoded += value.descriptor.canonical_type_name;
        encoded.push_back(':');
        std::string scalar;
        if (!RenderAggregateScalarPayload(value, &scalar)) {
          *diagnostic = Refusal(
              "QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
              "ARRAY_AGG scalar rendering overflowed");
          return {};
        }
        encoded += scalar;
      }
    }
    encoded.push_back(']');
    return AggregateValue(request.result_column, std::move(encoded));
  }
  if (function == CanonicalAggregateFunction::json_agg) {
    if (state.transition_count == 0) return AggregateNull(request.result_column);
    std::string encoded;
    encoded.reserve(plan.output_bytes);
    encoded.push_back('[');
    for (std::size_t index = 0; index < state.text_values.size(); ++index) {
      if (index != 0) encoded.push_back(',');
      encoded += state.text_values[index];
    }
    encoded.push_back(']');
    return AggregateValue(request.result_column, std::move(encoded));
  }
  if (function == CanonicalAggregateFunction::json_object_agg) {
    if (state.transition_count == 0) return AggregateNull(request.result_column);
    std::string encoded = "{";
    encoded.reserve(plan.output_bytes);
    for (std::size_t index = 0; index < state.json_object_values.size();
         ++index) {
      if (index != 0) encoded.push_back(',');
      AppendAggregateJsonEscaped(state.json_object_values[index].first,
                                 &encoded);
      encoded.push_back(':');
      encoded += state.json_object_values[index].second;
    }
    encoded.push_back('}');
    return AggregateValue(request.result_column, std::move(encoded));
  }
  if (function == CanonicalAggregateFunction::string_agg ||
      function == CanonicalAggregateFunction::listagg) {
    if (state.text_values.empty()) return AggregateNull(request.result_column);
    const auto allocation_refusal = [&]() {
      *diagnostic = Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "string aggregate result allocation was refused");
      return EngineTypedValue{};
    };
    if (function == CanonicalAggregateFunction::string_agg ||
        request.listagg_overflow_mode == CanonicalListaggOverflowMode::none) {
      try {
        return AggregateValue(
            request.result_column,
            JoinAggregateTextValues(state.text_values,
                                    state.text_values.size(),
                                    request.aggregate_separator));
      } catch (const std::bad_alloc&) {
        return allocation_refusal();
      } catch (const std::length_error&) {
        return allocation_refusal();
      }
    }
    std::size_t full_text_size = 0;
    if (!AggregateTextValuesEncodedSize(
            state.text_values, state.text_values.size(),
            request.aggregate_separator, &full_text_size)) {
      *diagnostic = Refusal(
          "QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1",
          "LISTAGG result size overflowed before materialization");
      return {};
    }
    if (full_text_size <= request.listagg_max_output_bytes) {
      try {
        return AggregateValue(
            request.result_column,
            JoinAggregateTextValues(state.text_values,
                                    state.text_values.size(),
                                    request.aggregate_separator));
      } catch (const std::bad_alloc&) {
        return allocation_refusal();
      } catch (const std::length_error&) {
        return allocation_refusal();
      }
    }
    if (request.listagg_overflow_mode ==
        CanonicalListaggOverflowMode::error) {
      *diagnostic = Refusal("QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1",
                            "LISTAGG result exceeds its bound output bytes");
      return {};
    }
    const auto decimal_digits = [](std::size_t value) {
      std::size_t digits = 1;
      while (value >= 10) {
        value /= 10;
        ++digits;
      }
      return digits;
    };
    const auto suffix_size = [&](const std::size_t truncated_count,
                                 std::size_t* size) {
      if (size == nullptr) return false;
      *size = request.listagg_truncation_indicator.empty()
                  ? 3
                  : request.listagg_truncation_indicator.size();
      if (!request.listagg_with_count) return true;
      const auto digits = decimal_digits(truncated_count);
      if (*size > std::numeric_limits<std::size_t>::max() - 2 ||
          *size + 2 >
              std::numeric_limits<std::size_t>::max() - digits) {
        return false;
      }
      *size += 2 + digits;
      return true;
    };
    std::size_t prefix_size = full_text_size;
    std::size_t selected_retained = 0;
    bool selected_candidate = false;
    for (std::size_t retained = state.text_values.size(); retained > 0;
         --retained) {
      const auto truncated = state.text_values.size() - retained;
      if (truncated != 0) {
        std::size_t current_suffix_size = 0;
        std::size_t candidate_size = prefix_size;
        if (!suffix_size(truncated, &current_suffix_size) ||
            candidate_size >
                std::numeric_limits<std::size_t>::max() -
                    request.aggregate_separator.size()) {
          *diagnostic = Refusal(
              "QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1",
              "LISTAGG truncation size overflowed");
          return {};
        }
        candidate_size += request.aggregate_separator.size();
        if (candidate_size >
            std::numeric_limits<std::size_t>::max() - current_suffix_size) {
          *diagnostic = Refusal(
              "QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1",
              "LISTAGG truncation size overflowed");
          return {};
        }
        candidate_size += current_suffix_size;
        if (candidate_size <= request.listagg_max_output_bytes) {
          selected_retained = retained;
          selected_candidate = true;
          break;
        }
      }
      if (retained > 1) {
        const auto removed = state.text_values[retained - 1].size();
        if (prefix_size < removed ||
            prefix_size - removed < request.aggregate_separator.size()) {
          *diagnostic = Refusal(
              "QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1",
              "LISTAGG prefix accounting diverged");
          return {};
        }
        prefix_size -= removed + request.aggregate_separator.size();
      }
    }
    const auto make_suffix = [&](const std::size_t truncated_count) {
      auto value = request.listagg_truncation_indicator.empty()
                       ? std::string("...")
                       : request.listagg_truncation_indicator;
      if (request.listagg_with_count) {
        value += "(" + std::to_string(truncated_count) + ")";
      }
      return value;
    };
    if (selected_candidate) {
      try {
        auto candidate = JoinAggregateTextValues(
            state.text_values, selected_retained,
            request.aggregate_separator);
        candidate += request.aggregate_separator;
        candidate +=
            make_suffix(state.text_values.size() - selected_retained);
        return AggregateValue(request.result_column, std::move(candidate));
      } catch (const std::bad_alloc&) {
        return allocation_refusal();
      } catch (const std::length_error&) {
        return allocation_refusal();
      }
    }
    std::size_t suffix_only_size = 0;
    if (suffix_size(state.text_values.size(), &suffix_only_size) &&
        suffix_only_size <= request.listagg_max_output_bytes) {
      try {
        return AggregateValue(
            request.result_column, make_suffix(state.text_values.size()));
      } catch (const std::bad_alloc&) {
        return allocation_refusal();
      } catch (const std::length_error&) {
        return allocation_refusal();
      }
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
    // MODE is an ordered-set aggregate. Transitions reached this state in the
    // validated WITHIN GROUP order, and worker merges preserve contiguous
    // transition order, so the first maximum is the canonical typed tie.
    std::size_t selected = 0;
    for (std::size_t index = 1; index < state.frequency_values.size(); ++index) {
      bool replace = state.frequency_values[index].second >
                     state.frequency_values[selected].second;
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
    const auto limit = std::min(
        static_cast<std::size_t>(decoded.value), plan.top_k_order.size());
    std::string encoded = "[";
    encoded.reserve(plan.output_bytes);
    for (std::size_t rank = 0; rank < limit; ++rank) {
      if (rank != 0) encoded.push_back(',');
      const auto index = plan.top_k_order[rank];
      std::string scalar;
      if (!RenderAggregateScalarPayload(
              state.frequency_values[index].first, &scalar)) {
        *diagnostic = Refusal(
            "QOW-DIAG-QRY-011-REGISTRY-OVERFLOW-V1",
            "APPROX_TOP_K scalar rendering overflowed");
        return {};
      }
      encoded += "{\"value\":";
      AppendAggregateJsonEscaped(scalar, &encoded);
      encoded += ",\"count\":";
      encoded += std::to_string(state.frequency_values[index].second);
      encoded.push_back('}');
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

EngineTypedValue FinalizeCanonicalAggregateCore(
    const CanonicalAggregateRuntimeRequest& request,
    const CanonicalAggregateCoreState& state,
    DescriptorRuntimeDiagnostic* diagnostic,
    CanonicalAggregateFinalizationReceipt* receipt,
    const std::size_t maximum_final_output_bytes,
    const std::size_t maximum_finalization_workspace_bytes,
    const std::size_t maximum_live_finalization_bytes) {
  if (diagnostic == nullptr || receipt == nullptr) return {};
  *receipt = {};
  CanonicalAggregateFinalizationPlan plan;
  try {
    if (!PlanCanonicalAggregateFinalization(
            request, state, &plan, diagnostic,
            maximum_finalization_workspace_bytes,
            maximum_live_finalization_bytes)) {
      return {};
    }
  } catch (const std::bad_alloc&) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate finalization planning allocation was refused");
    return {};
  } catch (const std::length_error&) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate finalization planning length exceeded its resource contract");
    return {};
  }
  std::size_t live_finalization_bytes = plan.output_bytes;
  if (!CheckedAggregateFinalizationAdd(&live_finalization_bytes,
                                        plan.peak_workspace_bytes)) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate finalization peak memory overflowed before allocation");
    return {};
  }
  if (plan.output_bytes > maximum_final_output_bytes ||
      plan.peak_workspace_bytes > maximum_finalization_workspace_bytes ||
      live_finalization_bytes > maximum_live_finalization_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate final output or finalization workspace bound is exceeded");
    return {};
  }
  EngineTypedValue value;
  try {
    value = FinalizeCanonicalAggregateCoreUnchecked(request, state,
                                                    diagnostic, plan);
  } catch (const std::bad_alloc&) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate finalization allocation was refused");
    return {};
  } catch (const std::length_error&) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate finalization length exceeded its resource contract");
    return {};
  }
  if (!diagnostic->ok) return {};
  std::size_t actual_output_bytes = 0;
  if (!AggregateTypedValuePayloadBytes(value, &actual_output_bytes) ||
      actual_output_bytes > maximum_final_output_bytes ||
      actual_output_bytes > plan.output_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate final output diverged from its preallocation bound");
    return {};
  }
  receipt->output_bytes = actual_output_bytes;
  receipt->peak_workspace_bytes = plan.peak_workspace_bytes;
  return value;
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

bool AggregateDistinctKey(const AggregateTransitionValuesView& values,
                          std::string* key,
                          DescriptorRuntimeDiagnostic* diagnostic) {
  namespace dt = scratchbird::core::datatypes;
  if (key == nullptr || diagnostic == nullptr) return false;
  key->clear();
  key->reserve(values.size * 64);
  for (std::size_t index = 0; index < values.size; ++index) {
    const auto& value = values[index];
    AppendAggregateDistinctKeyField(
        key, value.descriptor.descriptor_uuid.canonical);
    AppendAggregateDistinctKeyField(key,
                                    value.descriptor.canonical_type_name);
    key->push_back(static_cast<char>(value.state));
    key->push_back(value.is_null ? 1 : 0);
    std::string canonical_payload = value.encoded_value;
    std::string_view auxiliary_binary;
    if (value.state == EngineValueState::value && !value.is_null) {
      const auto type_id = dt::CanonicalTypeIdFromStableName(
          value.descriptor.canonical_type_name);
      const bool canonical_integer =
          type_id == dt::CanonicalTypeId::int8 ||
          type_id == dt::CanonicalTypeId::int16 ||
          type_id == dt::CanonicalTypeId::int32 ||
          type_id == dt::CanonicalTypeId::int64 ||
          type_id == dt::CanonicalTypeId::int128 ||
          type_id == dt::CanonicalTypeId::uint8 ||
          type_id == dt::CanonicalTypeId::uint16 ||
          type_id == dt::CanonicalTypeId::uint32 ||
          type_id == dt::CanonicalTypeId::uint64 ||
          type_id == dt::CanonicalTypeId::uint128;
      const bool canonical_decimal =
          type_id == dt::CanonicalTypeId::decimal ||
          type_id == dt::CanonicalTypeId::decimal_float;
      if (canonical_integer) {
        dt::DatatypeCastRequest request;
        request.value.type_id = type_id;
        request.value.encoded_value = value.encoded_value;
        request.value.is_null = false;
        request.target_type_id = type_id;
        request.explicit_cast = true;
        const auto cast = dt::CastDatatypeValue(request);
        if (!cast.ok()) {
          *diagnostic = Refusal(
              "QOW-DIAG-QRY-011-REGISTRY-DISTINCT-V1",
              cast.diagnostic.diagnostic_code);
          return false;
        }
        canonical_payload = cast.value.encoded_value;
      } else if (canonical_decimal) {
        dt::DatatypeSortKeyRequest request;
        request.value.type_id = type_id;
        request.value.encoded_value = value.encoded_value;
        request.value.is_null = false;
        const auto sorted = dt::MakeDatatypeSortKey(request);
        if (!sorted.ok()) {
          *diagnostic = Refusal(
              "QOW-DIAG-QRY-011-REGISTRY-DISTINCT-V1",
              sorted.diagnostic.diagnostic_code);
          return false;
        }
        canonical_payload = sorted.sort_key;
      } else if (value.descriptor.canonical_type_name == "boolean" ||
                 value.descriptor.canonical_type_name == "bool") {
        const auto decoded = DecodeBoolValue(value);
        if (decoded.ok()) canonical_payload = decoded.value ? "true" : "false";
      } else if (value.descriptor.canonical_type_name == "real64" ||
                 value.descriptor.canonical_type_name == "double" ||
                 value.descriptor.canonical_type_name == "double precision") {
        const auto decoded = DecodeReal64Value(value);
        if (decoded.ok()) canonical_payload = FormatAggregateReal(decoded.value);
      } else if ((value.descriptor.canonical_type_name == "binary" ||
                  value.descriptor.canonical_type_name == "blob" ||
                  value.descriptor.canonical_type_name == "bytes") &&
                 !value.binary_value.empty()) {
        canonical_payload.assign(
            reinterpret_cast<const char*>(value.binary_value.data()),
            value.binary_value.size());
      } else if (!value.binary_value.empty()) {
        auxiliary_binary = std::string_view(
            reinterpret_cast<const char*>(value.binary_value.data()),
            value.binary_value.size());
      }
    }
    AppendAggregateDistinctKeyField(key, canonical_payload);
    AppendAggregateDistinctKeyField(key, auxiliary_binary);
  }
  return true;
}

struct PreparedAggregateTransitions {
  std::vector<std::size_t> transitions;
  std::size_t retained_bytes = 0;
  std::size_t input_row_count = 0;
  std::size_t filtered_row_count = 0;
  std::size_t distinct_tuple_count = 0;
  std::size_t order_comparison_count = 0;
  bool aggregate_order_applied = false;
};

AggregateTransitionValuesView BindAggregateTransitionValues(
    const CanonicalAggregateRuntimeRequest& request,
    const DescriptorBatch& input_batch,
    const std::size_t input_row) {
  AggregateTransitionValuesView values;
  values.size = request.value_columns.size();
  for (std::size_t index = 0; index < values.size; ++index) {
    values.values[index] =
        &input_batch.rows[input_row]
             .values[request.value_columns[index]];
  }
  return values;
}

bool AggregateDistinctKeyAllocationBound(
    const AggregateTransitionValuesView& values,
    std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 34;
  for (std::size_t index = 0; index < values.size; ++index) {
    const auto& value = values[index];
    std::size_t payload_bytes = value.encoded_value.size();
    if (!CheckedAggregateFinalizationAdd(&payload_bytes,
                                          value.binary_value.size()) ||
        !CheckedAggregateFinalizationMultiply(payload_bytes, 6,
                                                &payload_bytes) ||
        !CheckedAggregateFinalizationAdd(
            bytes, value.descriptor.descriptor_uuid.canonical.size()) ||
        !CheckedAggregateFinalizationAdd(
            bytes, value.descriptor.canonical_type_name.size()) ||
        !CheckedAggregateFinalizationAdd(bytes, payload_bytes)) {
      return false;
    }
  }
  return true;
}

bool PrepareCanonicalAggregateTransitions(
    const CanonicalAggregateRuntimeRequest& request,
    const DescriptorBatch& input_batch,
    const std::size_t maximum_live_bytes,
    PreparedAggregateTransitions* prepared,
    DescriptorRuntimeDiagnostic* diagnostic) {
  using scratchbird::engine::internal_api::EnginePredicateConsumer;
  using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;
  if (prepared == nullptr || diagnostic == nullptr) return false;
  *prepared = {};
  prepared->input_row_count = input_batch.rows.size();
  std::size_t admitted_row_count = 0;
  for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
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
    ++admitted_row_count;
  }
  if (!CheckedAggregateFinalizationMultiply(admitted_row_count,
                                              sizeof(std::size_t),
                                              &prepared->retained_bytes) ||
      prepared->retained_bytes > maximum_live_bytes) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate transition ordinal workspace exceeds the selected-node grant");
    return false;
  }
  prepared->transitions.reserve(admitted_row_count);
  std::vector<std::string> distinct_keys;
  std::size_t distinct_container_bytes = 0;
  if (request.distinct &&
      (!CheckedAggregateFinalizationMultiply(
           admitted_row_count, sizeof(std::string),
           &distinct_container_bytes) ||
       distinct_container_bytes >
           maximum_live_bytes - prepared->retained_bytes)) {
    *diagnostic = Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate DISTINCT key workspace exceeds the selected-node grant");
    return false;
  }
  if (request.distinct) distinct_keys.reserve(admitted_row_count);
  std::size_t retained_distinct_key_bytes = 0;
  for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
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
    const auto values =
        BindAggregateTransitionValues(request, input_batch, row);
    if (request.distinct) {
      std::size_t key_allocation_bound = 0;
      if (!AggregateDistinctKeyAllocationBound(values,
                                                &key_allocation_bound) ||
          retained_distinct_key_bytes >
              maximum_live_bytes - prepared->retained_bytes -
                  distinct_container_bytes ||
          key_allocation_bound >
              maximum_live_bytes - prepared->retained_bytes -
                  distinct_container_bytes -
                  retained_distinct_key_bytes) {
        *diagnostic = Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "aggregate DISTINCT key exceeded the selected-node grant");
        return false;
      }
      std::string distinct_key;
      if (!AggregateDistinctKey(values, &distinct_key, diagnostic)) {
        return false;
      }
      if (std::find(distinct_keys.begin(), distinct_keys.end(),
                    distinct_key) != distinct_keys.end()) {
        continue;
      }
      retained_distinct_key_bytes += key_allocation_bound;
      distinct_keys.push_back(std::move(distinct_key));
      if (distinct_keys.size() > request.maximum_distinct_value_count) {
        *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "aggregate DISTINCT bound is exceeded");
        return false;
      }
    }
    prepared->transitions.push_back(row);
  }
  prepared->distinct_tuple_count = request.distinct ? distinct_keys.size() : 0;

  if (request.aggregate_order_terms.empty()) return true;
  const auto row_count = prepared->transitions.size();
  const auto term_count = request.aggregate_order_terms.size();
  std::size_t comparison_bound = 0;
  if (request.maximum_order_comparison_count == 0 ||
      (row_count > 1 &&
       !CheckedAggregateFinalizationMultiply(
           row_count, row_count - 1, &comparison_bound)) ||
      (comparison_bound /= 2,
       !CheckedAggregateFinalizationMultiply(comparison_bound, term_count,
                                               &comparison_bound)) ||
      comparison_bound > request.maximum_order_comparison_count) {
    *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate order comparison bound is exceeded");
    return false;
  }
  for (std::size_t index = 1; index < row_count; ++index) {
    const auto moving_row = prepared->transitions[index];
    std::size_t insertion = index;
    while (insertion != 0) {
      int comparison = 0;
      for (const auto& term : request.aggregate_order_terms) {
        const auto compared = CompareCanonicalDescriptorOrderValues(
            input_batch.rows[moving_row]
                .values[term.column],
            input_batch.rows[prepared->transitions[insertion - 1]]
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
      if (comparison >= 0) break;
      prepared->transitions[insertion] =
          prepared->transitions[insertion - 1];
      --insertion;
    }
    prepared->transitions[insertion] = moving_row;
  }
  prepared->aggregate_order_applied = true;
  return true;
}

bool BuildCanonicalAggregateCoreState(
    const CanonicalAggregateRuntimeRequest& request,
    const DescriptorBatch& input_batch,
    const std::vector<std::size_t>& transitions,
    const std::size_t maximum_state_bytes,
    CanonicalAggregateCoreState* state,
    DescriptorRuntimeDiagnostic* diagnostic) {
  if (state == nullptr || diagnostic == nullptr) return false;
  *state = {};
  if (EstimateCanonicalAggregateStateBytes(*state) > maximum_state_bytes) {
    *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate base state byte bound is exceeded");
    return false;
  }
  const auto state_within_bound = [&](const CanonicalAggregateCoreState& value) {
    return EstimateCanonicalAggregateStateBytes(value) <=
           maximum_state_bytes;
  };
  if (request.forced_strategy == CanonicalAggregateExecutionStrategy::serial) {
    if (!ReserveCanonicalAggregateCoreState(
            request.descriptor, transitions.size(), maximum_state_bytes,
            state, diagnostic)) {
      return false;
    }
    for (const auto input_row : transitions) {
      if (!TransitionCanonicalAggregateCoreBounded(
          request.descriptor,
              BindAggregateTransitionValues(request, input_batch, input_row),
              maximum_state_bytes, state, diagnostic)) {
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
  if (!ReserveCanonicalAggregateCoreState(
          request.descriptor, transitions.size(), maximum_state_bytes,
          &left, diagnostic)) {
    return false;
  }
  const auto left_reserved_bytes = EstimateCanonicalAggregateStateBytes(left);
  if (left_reserved_bytes > maximum_state_bytes ||
      !ReserveCanonicalAggregateCoreState(
          request.descriptor, transitions.size() - split,
          maximum_state_bytes - left_reserved_bytes, &right,
          diagnostic)) {
    return false;
  }
  for (std::size_t index = 0; index < transitions.size(); ++index) {
    auto* partition = index < split ? &left : &right;
    const auto* other = index < split ? &right : &left;
    const auto other_state_bytes =
        EstimateCanonicalAggregateStateBytes(*other);
    if (other_state_bytes > maximum_state_bytes ||
        !TransitionCanonicalAggregateCoreBounded(
            request.descriptor,
            BindAggregateTransitionValues(
                request, input_batch, transitions[index]),
            maximum_state_bytes - other_state_bytes, partition,
            diagnostic)) {
      return false;
    }
    const auto left_bytes = EstimateCanonicalAggregateStateBytes(left);
    const auto right_bytes = EstimateCanonicalAggregateStateBytes(right);
    if (!state_within_bound(*partition) ||
        left_bytes > maximum_state_bytes ||
        right_bytes > maximum_state_bytes - left_bytes) {
      *diagnostic = Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "aggregate live partial-state byte bound is exceeded");
      return false;
    }
  }
  *state = std::move(left);
  left = {};
  if (!MergeCanonicalAggregateCoreBounded(
          request.descriptor, state, std::move(right),
          maximum_state_bytes, diagnostic)) {
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

bool FormatInt128StateText(const __int128 value,
                           std::array<char, 64>* storage,
                           std::string_view* text) {
  if (storage == nullptr || text == nullptr) return false;
  if (value == 0) {
    (*storage)[0] = '0';
    *text = std::string_view(storage->data(), 1);
    return true;
  }
  const bool negative = value < 0;
  unsigned __int128 magnitude = negative
                                    ? static_cast<unsigned __int128>(
                                          -(value + 1)) + 1
                                    : static_cast<unsigned __int128>(value);
  std::size_t length = 0;
  while (magnitude != 0) {
    if (length == storage->size()) return false;
    (*storage)[length++] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  }
  if (negative) {
    if (length == storage->size()) return false;
    (*storage)[length++] = '-';
  }
  std::reverse(storage->begin(), storage->begin() +
                                      static_cast<std::ptrdiff_t>(length));
  *text = std::string_view(storage->data(), length);
  return true;
}

class FixedAggregateStateStreamBuffer final : public std::streambuf {
 public:
  FixedAggregateStateStreamBuffer(char* begin, const std::size_t size) {
    setp(begin, begin + static_cast<std::ptrdiff_t>(size));
  }

  std::size_t size() const {
    return static_cast<std::size_t>(pptr() - pbase());
  }

 protected:
  int_type overflow(const int_type character) override {
    if (traits_type::eq_int_type(character, traits_type::eof())) {
      return traits_type::not_eof(character);
    }
    return traits_type::eof();
  }
};

bool FormatLongDoubleStateText(const long double value,
                              std::array<char, 192>* storage,
                              std::string_view* text) {
  if (storage == nullptr || text == nullptr) return false;
  FixedAggregateStateStreamBuffer buffer(storage->data(), storage->size());
  std::ostream stream(&buffer);
  stream << std::scientific
         << std::setprecision(std::numeric_limits<long double>::max_digits10)
         << value;
  if (!stream) return false;
  *text = std::string_view(storage->data(), buffer.size());
  return true;
}

bool AddSerializedStateStringSize(const std::size_t string_size,
                                  std::size_t* total) {
  return CheckedAggregateFinalizationAdd(total, sizeof(std::uint64_t)) &&
         CheckedAggregateFinalizationAdd(total, string_size);
}

bool AddSerializedStateValueSize(const EngineTypedValue& value,
                                 std::size_t* total) {
  return AddSerializedStateStringSize(
             value.descriptor.descriptor_uuid.canonical.size(), total) &&
         AddSerializedStateStringSize(
             value.descriptor.descriptor_kind.size(), total) &&
         AddSerializedStateStringSize(
             value.descriptor.canonical_type_name.size(), total) &&
         AddSerializedStateStringSize(
             value.descriptor.encoded_descriptor.size(), total) &&
         AddSerializedStateStringSize(value.encoded_value.size(), total) &&
         AddSerializedStateStringSize(value.binary_value.size(), total) &&
         CheckedAggregateFinalizationAdd(total, 2);
}

bool PlanCanonicalAggregateCoreStateSerialization(
    const CanonicalAggregateRuntimeRequest& request,
    const CanonicalAggregateCoreState& state,
    const std::size_t maximum_bytes,
    std::size_t* encoded_bytes) {
  if (encoded_bytes == nullptr || maximum_bytes == 0) return false;
  *encoded_bytes = 0;
  const auto add_u64 = [&]() {
    return CheckedAggregateFinalizationAdd(encoded_bytes,
                                            sizeof(std::uint64_t));
  };
  const auto add_byte = [&]() {
    return CheckedAggregateFinalizationAdd(encoded_bytes, 1);
  };
  const auto add_string = [&](const std::size_t size) {
    return AddSerializedStateStringSize(size, encoded_bytes);
  };
  std::array<char, 64> int_storage{};
  std::string_view int_text;
  if (!FormatInt128StateText(state.int64_sum, &int_storage, &int_text) ||
      !add_string(std::string_view("scratchbird.aggregate-state.v1").size()) ||
      !add_u64() || !add_u64() ||
      !add_string(request.descriptor.builtin_id.size()) ||
      !add_string(request.descriptor.function_uuid.size()) || !add_byte() ||
      !add_u64() || !add_u64() || !add_u64() ||
      !add_string(int_text.size())) {
    return false;
  }
  for (const auto value :
       {state.real_sum, state.numeric_mean, state.numeric_m2,
        state.pair_mean_x, state.pair_mean_y, state.pair_m2_x,
        state.pair_m2_y, state.pair_comoment}) {
    std::array<char, 192> storage{};
    std::string_view text;
    if (!std::isfinite(value) ||
        !FormatLongDoubleStateText(value, &storage, &text) ||
        !add_string(text.size())) {
      return false;
    }
  }
  if (!add_byte() || !add_byte() || !add_byte() ||
      (state.extremum.has_value() &&
       !AddSerializedStateValueSize(*state.extremum, encoded_bytes)) ||
      !add_u64()) {
    return false;
  }
  for (const auto& value : state.collection_values) {
    if (!AddSerializedStateValueSize(value, encoded_bytes)) return false;
  }
  if (!add_u64()) return false;
  for (const auto& value : state.text_values) {
    if (!add_string(value.size())) return false;
  }
  if (!add_u64()) return false;
  for (const auto& [key, value] : state.json_object_values) {
    if (!add_string(key.size()) || !add_string(value.size())) return false;
  }
  if (!add_u64()) return false;
  for (const auto value : state.ordered_numeric_values) {
    std::array<char, 192> storage{};
    std::string_view text;
    if (!std::isfinite(value) ||
        !FormatLongDoubleStateText(value, &storage, &text) ||
        !add_string(text.size())) {
      return false;
    }
  }
  if (!add_u64()) return false;
  for (const auto& value : state.approximate_distinct_values) {
    if (!add_string(value.size())) return false;
  }
  if (!add_u64()) return false;
  for (const auto& [value, count] : state.frequency_values) {
    (void)count;
    if (!AddSerializedStateValueSize(value, encoded_bytes) || !add_u64()) {
      return false;
    }
  }
  return *encoded_bytes <= maximum_bytes;
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
  std::size_t planned_bytes = 0;
  if (!PlanCanonicalAggregateCoreStateSerialization(
          request, state, maximum_bytes, &planned_bytes)) {
    return false;
  }
  bytes->reserve(planned_bytes);
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
  std::array<char, 64> int_storage{};
  std::string_view int_text;
  if (!FormatInt128StateText(state.int64_sum, &int_storage, &int_text)) {
    return false;
  }
  AppendStateString(bytes, int_text);
  for (const auto value :
       {state.real_sum, state.numeric_mean, state.numeric_m2,
        state.pair_mean_x, state.pair_mean_y, state.pair_m2_x,
        state.pair_m2_y, state.pair_comoment}) {
    std::array<char, 192> storage{};
    std::string_view text;
    if (!std::isfinite(value) ||
        !FormatLongDoubleStateText(value, &storage, &text)) {
      return false;
    }
    AppendStateString(bytes, text);
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
    std::array<char, 192> storage{};
    std::string_view text;
    if (!std::isfinite(value) ||
        !FormatLongDoubleStateText(value, &storage, &text)) {
      return false;
    }
    AppendStateString(bytes, text);
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
  return bytes->size() == planned_bytes;
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

  bool ReadStringView(std::string_view* value) {
    std::size_t size = 0;
    if (value == nullptr || !ReadSize(&size) || size > Remaining() ||
        size > maximum_bytes_) {
      return false;
    }
    *value = std::string_view(
        reinterpret_cast<const char*>(bytes_.data() + offset_), size);
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

bool PlanCanonicalAggregateCoreStateDeserialization(
    const CanonicalAggregateRuntimeRequest& request,
    const AggregateStateBytes& bytes,
    const std::size_t maximum_serialized_bytes,
    std::size_t* planned_state_bytes) {
  if (planned_state_bytes == nullptr || bytes.empty() ||
      bytes.size() > maximum_serialized_bytes) {
    return false;
  }
  AggregateStateReader reader(bytes, maximum_serialized_bytes);
  std::string_view text;
  std::uint64_t abi_version = 0;
  std::uint64_t function = 0;
  std::uint8_t count_star = 0;
  std::uint64_t strategy = 0;
  std::size_t transition_count = 0;
  std::size_t non_null_count = 0;
  if (!reader.ReadStringView(&text) ||
      text != "scratchbird.aggregate-state.v1" ||
      !reader.ReadU64(&abi_version) ||
      abi_version != request.descriptor.abi_version ||
      !reader.ReadU64(&function) ||
      function != static_cast<std::uint8_t>(request.descriptor.function) ||
      !reader.ReadStringView(&text) || text != request.descriptor.builtin_id ||
      !reader.ReadStringView(&text) || text != request.descriptor.function_uuid ||
      !reader.ReadByte(&count_star) || count_star > 1 ||
      (count_star != 0) != request.descriptor.count_star ||
      !reader.ReadU64(&strategy) ||
      strategy != static_cast<std::uint8_t>(request.forced_strategy) ||
      !reader.ReadSize(&transition_count) ||
      !reader.ReadSize(&non_null_count) || non_null_count > transition_count ||
      !reader.ReadStringView(&text)) {
    return false;
  }
  for (std::size_t index = 0; index < 8; ++index) {
    if (!reader.ReadStringView(&text)) return false;
  }

  std::size_t element_bytes = 0;
  const auto function_kind = request.descriptor.function;
  if (function_kind == CanonicalAggregateFunction::array_agg) {
    element_bytes = sizeof(EngineTypedValue);
  } else if (function_kind == CanonicalAggregateFunction::json_agg ||
             function_kind == CanonicalAggregateFunction::string_agg ||
             function_kind == CanonicalAggregateFunction::listagg ||
             function_kind ==
                 CanonicalAggregateFunction::approx_count_distinct) {
    element_bytes = sizeof(std::string);
  } else if (function_kind ==
             CanonicalAggregateFunction::json_object_agg) {
    element_bytes = sizeof(std::pair<std::string, std::string>);
  } else if (IsCanonicalHypotheticalSetFunction(function_kind) ||
             IsCanonicalQuantileFunction(function_kind)) {
    element_bytes = sizeof(long double);
  } else if (function_kind == CanonicalAggregateFunction::mode ||
             function_kind == CanonicalAggregateFunction::approx_top_k) {
    element_bytes = sizeof(std::pair<EngineTypedValue, std::size_t>);
  }
  *planned_state_bytes = sizeof(CanonicalAggregateCoreState);
  std::size_t structural_bytes = 0;
  if (!CheckedAggregateFinalizationMultiply(transition_count, element_bytes,
                                             &structural_bytes) ||
      !CheckedAggregateFinalizationAdd(planned_state_bytes,
                                       structural_bytes)) {
    return false;
  }
  const auto add_dynamic_string = [&]() {
    if (!reader.ReadStringView(&text)) return false;
    return CheckedAggregateFinalizationAdd(planned_state_bytes,
                                           text.size());
  };
  const auto read_value = [&]() {
    for (std::size_t field = 0; field < 5; ++field) {
      if (!add_dynamic_string()) return false;
    }
    std::size_t binary_size = 0;
    if (!reader.ReadSize(&binary_size) || binary_size > reader.Remaining() ||
        !CheckedAggregateFinalizationAdd(planned_state_bytes,
                                         binary_size)) {
      return false;
    }
    for (std::size_t byte = 0; byte < binary_size; ++byte) {
      std::uint8_t ignored = 0;
      if (!reader.ReadByte(&ignored)) return false;
    }
    std::uint8_t is_null = 0;
    std::uint8_t state = 0;
    return reader.ReadByte(&is_null) && is_null <= 1 &&
           reader.ReadByte(&state) &&
           state <= static_cast<std::uint8_t>(EngineValueState::protected_value);
  };
  std::uint8_t flag = 0;
  if (!reader.ReadByte(&flag) || flag > 1 ||
      !reader.ReadByte(&flag) || flag > 1) {
    return false;
  }
  std::uint8_t has_extremum = 0;
  if (!reader.ReadByte(&has_extremum) || has_extremum > 1 ||
      (has_extremum != 0 && !read_value())) {
    return false;
  }
  const auto read_count = [&](std::size_t* count, const bool admitted) {
    return reader.ReadSize(count) && *count <= transition_count &&
           (admitted || *count == 0);
  };
  std::size_t count = 0;
  if (!read_count(&count,
                  function_kind == CanonicalAggregateFunction::array_agg)) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (!read_value()) return false;
  }
  if (!read_count(&count,
                  function_kind == CanonicalAggregateFunction::json_agg ||
                      function_kind == CanonicalAggregateFunction::string_agg ||
                      function_kind == CanonicalAggregateFunction::listagg)) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (!add_dynamic_string()) return false;
  }
  if (!read_count(
          &count,
          function_kind == CanonicalAggregateFunction::json_object_agg)) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (!add_dynamic_string() || !add_dynamic_string()) return false;
  }
  if (!read_count(&count,
                  IsCanonicalHypotheticalSetFunction(function_kind) ||
                      IsCanonicalQuantileFunction(function_kind))) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (!reader.ReadStringView(&text)) return false;
  }
  if (!read_count(
          &count,
          function_kind ==
              CanonicalAggregateFunction::approx_count_distinct)) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (!add_dynamic_string()) return false;
  }
  if (!read_count(&count,
                  function_kind == CanonicalAggregateFunction::mode ||
                      function_kind ==
                          CanonicalAggregateFunction::approx_top_k)) {
    return false;
  }
  for (std::size_t index = 0; index < count; ++index) {
    std::size_t ignored_count = 0;
    if (!read_value() || !reader.ReadSize(&ignored_count)) return false;
  }
  return reader.Remaining() == 0;
}

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

bool ParseLongDoubleStateText(const std::string_view text,
                              long double* value) {
  if (value == nullptr || text.empty()) return false;
  const auto parsed = std::from_chars(
      text.data(), text.data() + text.size(), *value,
      std::chars_format::scientific);
  return parsed.ec == std::errc{} &&
         parsed.ptr == text.data() + text.size() && std::isfinite(*value);
}

bool DeserializeCanonicalAggregateCoreState(
    const CanonicalAggregateRuntimeRequest& request,
    const AggregateStateBytes& bytes,
    const std::size_t maximum_serialized_bytes,
    const std::size_t maximum_live_state_bytes,
    CanonicalAggregateCoreState* state) {
  std::size_t planned_state_bytes = 0;
  if (state == nullptr ||
      !PlanCanonicalAggregateCoreStateDeserialization(
          request, bytes, maximum_serialized_bytes,
          &planned_state_bytes) ||
      planned_state_bytes > maximum_live_state_bytes) {
    return false;
  }
  *state = {};
  AggregateStateReader reader(bytes, maximum_serialized_bytes);
  std::string_view text;
  std::uint64_t abi_version = 0;
  std::uint64_t function = 0;
  std::uint8_t count_star = 0;
  std::uint64_t strategy = 0;
  if (!reader.ReadStringView(&text) ||
      text != "scratchbird.aggregate-state.v1" ||
      !reader.ReadU64(&abi_version) ||
      abi_version != request.descriptor.abi_version ||
      !reader.ReadU64(&function) ||
      function != static_cast<std::uint8_t>(request.descriptor.function) ||
      !reader.ReadStringView(&text) || text != request.descriptor.builtin_id ||
      !reader.ReadStringView(&text) ||
      text != request.descriptor.function_uuid ||
      !reader.ReadByte(&count_star) || count_star > 1 ||
      (count_star != 0) != request.descriptor.count_star ||
      !reader.ReadU64(&strategy) ||
      strategy != static_cast<std::uint8_t>(request.forced_strategy) ||
      !reader.ReadSize(&state->transition_count) ||
      !reader.ReadSize(&state->non_null_count) ||
      !reader.ReadStringView(&text) ||
      !ParseInt128StateText(text, &state->int64_sum)) {
    return false;
  }
  DescriptorRuntimeDiagnostic capacity_diagnostic;
  if (!ReserveCanonicalAggregateCoreState(
          request.descriptor, state->transition_count,
          maximum_live_state_bytes, state,
          &capacity_diagnostic)) {
    return false;
  }
  for (auto* value :
       {&state->real_sum, &state->numeric_mean, &state->numeric_m2,
        &state->pair_mean_x, &state->pair_mean_y, &state->pair_m2_x,
        &state->pair_m2_y, &state->pair_comoment}) {
    if (!reader.ReadStringView(&text) ||
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
  const auto read_values = [&](std::vector<EngineTypedValue>* values,
                               const bool admitted) {
    std::size_t count = 0;
    if (!reader.ReadSize(&count) || count > maximum_serialized_bytes ||
        count > state->transition_count || (!admitted && count != 0)) {
      return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
      EngineTypedValue value;
      if (!reader.ReadValue(&value)) return false;
      values->push_back(std::move(value));
    }
    return true;
  };
  if (!read_values(
          &state->collection_values,
          request.descriptor.function ==
              CanonicalAggregateFunction::array_agg)) {
    return false;
  }
  const auto read_strings = [&](std::vector<std::string>* values,
                                const bool admitted) {
    std::size_t count = 0;
    if (!reader.ReadSize(&count) || count > maximum_serialized_bytes ||
        count > state->transition_count || (!admitted && count != 0)) {
      return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
      std::string value;
      if (!reader.ReadString(&value)) return false;
      values->push_back(std::move(value));
    }
    return true;
  };
  if (!read_strings(
          &state->text_values,
          request.descriptor.function == CanonicalAggregateFunction::json_agg ||
              request.descriptor.function ==
                  CanonicalAggregateFunction::string_agg ||
              request.descriptor.function ==
                  CanonicalAggregateFunction::listagg)) {
    return false;
  }
  std::size_t json_count = 0;
  if (!reader.ReadSize(&json_count) ||
      json_count > maximum_serialized_bytes ||
      json_count > state->transition_count ||
      (request.descriptor.function !=
           CanonicalAggregateFunction::json_object_agg &&
       json_count != 0)) {
    return false;
  }
  for (std::size_t index = 0; index < json_count; ++index) {
    std::string key;
    std::string value;
    if (!reader.ReadString(&key) || !reader.ReadString(&value)) return false;
    state->json_object_values.emplace_back(std::move(key), std::move(value));
  }
  std::size_t numeric_count = 0;
  if (!reader.ReadSize(&numeric_count) ||
      numeric_count > maximum_serialized_bytes) {
    return false;
  }
  if (numeric_count > state->transition_count ||
      (!(IsCanonicalHypotheticalSetFunction(request.descriptor.function) ||
         IsCanonicalQuantileFunction(request.descriptor.function)) &&
       numeric_count != 0)) {
    return false;
  }
  for (std::size_t index = 0; index < numeric_count; ++index) {
    long double value = 0.0L;
    if (!reader.ReadStringView(&text) ||
        !ParseLongDoubleStateText(text, &value)) {
      return false;
    }
    state->ordered_numeric_values.push_back(value);
  }
  if (!read_strings(
          &state->approximate_distinct_values,
          request.descriptor.function ==
              CanonicalAggregateFunction::approx_count_distinct)) {
    return false;
  }
  std::size_t frequency_count = 0;
  if (!reader.ReadSize(&frequency_count) ||
      frequency_count > maximum_serialized_bytes) {
    return false;
  }
  if (frequency_count > state->transition_count ||
      ((request.descriptor.function != CanonicalAggregateFunction::mode &&
        request.descriptor.function !=
            CanonicalAggregateFunction::approx_top_k) &&
       frequency_count != 0)) {
    return false;
  }
  for (std::size_t index = 0; index < frequency_count; ++index) {
    EngineTypedValue value;
    std::size_t count = 0;
    if (!reader.ReadValue(&value) || !reader.ReadSize(&count)) return false;
    state->frequency_values.emplace_back(std::move(value), count);
  }
  return reader.Remaining() == 0 &&
         state->non_null_count <= state->transition_count;
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
namespace {
CanonicalDescriptorCountResult ExecuteCanonicalDescriptorCountStarBound(
    const CanonicalDescriptorCountRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalDescriptorCountResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-AGGREGATE-PHYSICAL-ROUTE-V1",
        "COUNT(*) request carries conflicting owned execution carriers"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected aggregate node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
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
  for (const auto& node : execution_dag.nodes) {
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
      execution_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  if (execution_input_batch.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return refuse(Refusal("QOW-DIAG-QRY-007-AGGREGATE-OVERFLOW-V1",
                          "COUNT(*) exceeds int64 result width"));
  }

  EngineTypedValue count_value;
  count_value.descriptor = request.count_column.descriptor;
  count_value.encoded_value =
      std::to_string(execution_input_batch.rows.size());
  count_value.state = EngineValueState::value;
  result.output_batch.columns = {request.count_column};
  result.output_batch.rows = {{{std::move(count_value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorCountResult ExecuteCanonicalDescriptorCountStar(
    const CanonicalDescriptorCountRequest& request) {
  return ExecuteCanonicalDescriptorCountStarBound(
      request, request.physical_dag, request.input_batch, false);
}

CanonicalDescriptorCountResult ExecuteCanonicalDescriptorCountStar(
    const CanonicalDescriptorCountRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorCountStarBound(
      request, borrowed_execution_dag, borrowed_input_batch, true);
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

struct CanonicalAggregateExecutionMemoryScope {
  std::size_t retained_memory_bytes = 0;
  std::optional<std::size_t> maximum_final_output_bytes;
};

static CanonicalAggregateRuntimeResult ExecuteCanonicalAggregateRuntimeSelected(
    const CanonicalAggregateRuntimeRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers,
    const bool state_exchange_execution_context,
    const CanonicalAggregateExecutionMemoryScope memory_scope = {}) {
  CanonicalAggregateRuntimeResult result;
  result.descriptor = request.descriptor;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    const auto descriptor = result.descriptor;
    result = {};
    result.descriptor = descriptor;
    result.diagnostic = std::move(diagnostic);
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-PHYSICAL-V1",
        "aggregate request carries conflicting owned execution carriers"));
  }

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
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected aggregate node is not the root"));
  }
  const PhysicalNodeRecord* aggregate_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
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
  for (const auto& node : execution_dag.nodes) {
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
      execution_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  const auto node_memory_grant =
      AggregateNodeMemoryGrant(execution_dag, *aggregate_node);
  std::size_t input_payload_bytes = 0;
  std::size_t filter_memory_bytes = 0;
  if (!node_memory_grant.has_value() ||
      request.retained_memory_bytes > *node_memory_grant ||
      memory_scope.retained_memory_bytes >
          *node_memory_grant - request.retained_memory_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate selected-node retained memory exceeds its grant"));
  }
  const auto retained_memory_bytes =
      request.retained_memory_bytes + memory_scope.retained_memory_bytes;
  if (!node_memory_grant.has_value() ||
      !AggregateBatchPayloadBytes(execution_input_batch,
                                  &input_payload_bytes) ||
      (request.filter_truth_values.has_value() &&
       !CheckedAggregateFinalizationMultiply(
           request.filter_truth_values->size(),
           sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
           &filter_memory_bytes)) ||
      input_payload_bytes >
          *node_memory_grant - retained_memory_bytes ||
      filter_memory_bytes >
          *node_memory_grant - retained_memory_bytes -
              input_payload_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate selected-node memory grant is invalid or exhausted"));
  }
  const auto memory_after_fixed_inputs =
      *node_memory_grant - retained_memory_bytes - input_payload_bytes -
      filter_memory_bytes;
  const auto maximum_state_bytes =
      std::min(request.maximum_state_bytes, memory_after_fixed_inputs);
  auto maximum_final_output_bytes =
      request.maximum_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(request.maximum_final_output_bytes,
                     *node_memory_grant);
  if (memory_scope.maximum_final_output_bytes.has_value()) {
    maximum_final_output_bytes = std::min(
        maximum_final_output_bytes,
        *memory_scope.maximum_final_output_bytes);
  }
  const auto maximum_finalization_workspace_bytes =
      request.maximum_finalization_workspace_bytes == 0
          ? *node_memory_grant
          : std::min(request.maximum_finalization_workspace_bytes,
                     *node_memory_grant);

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
    if (column >= execution_input_batch.columns.size() ||
        execution_input_batch.columns[column].descriptor_id !=
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
      request.maximum_state_bytes == 0 || maximum_state_bytes == 0 ||
      execution_input_batch.rows.size() >
          request.maximum_transition_count) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "aggregate transition, state, or finalization bound is exceeded"));
  }
  if (request.filter_truth_values.has_value() &&
      request.filter_truth_values->size() !=
          execution_input_batch.rows.size()) {
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
    if (term.column >= execution_input_batch.columns.size()) {
      return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                            "aggregate order term column is unresolved"));
    }
    auto validation = ValidateCanonicalDescriptorOrderTerm(
        term, execution_input_batch.columns[term.column]);
    if (!validation.ok) return refuse(std::move(validation));
  }
  const auto function = request.descriptor.function;
  if (function == CanonicalAggregateFunction::mode &&
      (request.value_columns.size() != 1 ||
       request.value_expression_descriptor_ids.size() != 1 ||
       request.aggregate_order_terms.size() != 1 ||
       request.aggregate_order_terms.front().column !=
           request.value_columns.front() ||
       request.aggregate_order_terms.front().expression_descriptor_id !=
           request.value_expression_descriptor_ids.front())) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-ORDER-V1",
        "MODE value and sole WITHIN GROUP order term are not identically bound"));
  }
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
  if (!ValidateCanonicalAggregateInputType(
          request, execution_input_batch, &type_diagnostic)) {
    return refuse(std::move(type_diagnostic));
  }
  if (!ValidateCanonicalAggregateResultType(
          request, execution_input_batch, &type_diagnostic)) {
    return refuse(std::move(type_diagnostic));
  }

  PreparedAggregateTransitions prepared;
  DescriptorRuntimeDiagnostic state_diagnostic;
  if (!PrepareCanonicalAggregateTransitions(request, execution_input_batch,
                                            memory_after_fixed_inputs,
                                            &prepared,
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
  if (prepared.retained_bytes > memory_after_fixed_inputs) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate transition workspace exhausted the selected-node grant"));
  }
  const auto live_state_limit = std::min(
      maximum_state_bytes,
      memory_after_fixed_inputs - prepared.retained_bytes);
  if (!BuildCanonicalAggregateCoreState(
          request, execution_input_batch, prepared.transitions,
          live_state_limit, &state,
          &state_diagnostic)) {
    return refuse(std::move(state_diagnostic));
  }

  const auto state_bytes = EstimateCanonicalAggregateStateBytes(state);
  if (state_bytes >
      memory_after_fixed_inputs - prepared.retained_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate state exhausted the selected-node memory grant"));
  }
  result.transition_row_indices = std::move(prepared.transitions);
  const auto live_finalization_bytes =
      memory_after_fixed_inputs - prepared.retained_bytes - state_bytes;

  CanonicalAggregateFinalizationReceipt finalization_receipt;
  auto value = FinalizeCanonicalAggregateCore(
      request, state, &state_diagnostic, &finalization_receipt,
      maximum_final_output_bytes,
      maximum_finalization_workspace_bytes,
      live_finalization_bytes);
  if (!state_diagnostic.ok) return refuse(std::move(state_diagnostic));
  result.output_batch.columns = {request.result_column};
  result.output_batch.rows = {{{std::move(value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, aggregate_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.executed_strategy = request.forced_strategy;
  result.transition_count = state.transition_count;
  result.non_null_transition_count = state.non_null_count;
  result.state_bytes = state_bytes;
  result.final_output_bytes = finalization_receipt.output_bytes;
  result.peak_finalization_workspace_bytes =
      finalization_receipt.peak_workspace_bytes;
  result.direct_argument_count = request.direct_arguments.size();
  result.every_descriptor_field_consumed = true;
  result.shared_state_authority_used = true;
  result.authority.engine_mga_snapshot_bound = true;
  result.authority.owns_transaction_finality = false;
  result.authority.owns_recovery = false;
  result.authority.owns_parser_execution = false;
  result.authority.owns_visibility_outside_engine_mga = false;
  result.authority.wal_is_transaction_or_recovery_authority = false;
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = aggregate_node->physical_node_id;
  result.causal_counter_id = aggregate_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

CanonicalAggregateRuntimeResult ExecuteCanonicalAggregateRuntime(
    const CanonicalAggregateRuntimeRequest& request) {
  return ExecuteCanonicalAggregateRuntimeSelected(
      request, request.physical_dag, request.input_batch, false, false, {});
}

CanonicalAggregateRuntimeResult ExecuteCanonicalAggregateRuntime(
    const CanonicalAggregateRuntimeRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalAggregateRuntimeSelected(
      request, borrowed_execution_dag, borrowed_input_batch, true, false, {});
}

CanonicalAggregateRuntimeResult
ExecuteCanonicalAggregateRuntimeWithFinalOutputCeiling(
    const CanonicalAggregateRuntimeRequest& request,
    const std::size_t exact_final_output_ceiling) {
  return ExecuteCanonicalAggregateRuntimeSelected(
      request, request.physical_dag, request.input_batch, false, false,
      {0, exact_final_output_ceiling});
}

CanonicalAggregateRuntimeResult
ExecuteCanonicalAggregateRuntimeWithFinalOutputCeiling(
    const CanonicalAggregateRuntimeRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch,
    const std::size_t exact_final_output_ceiling) {
  return ExecuteCanonicalAggregateRuntimeSelected(
      request, borrowed_execution_dag, borrowed_input_batch, true, false,
      {0, exact_final_output_ceiling});
}

// QOW-SOURCE-QRY-011-REGISTRY-STATE-SPILL-V1
// Serialize the complete versioned aggregate transition state, spill and
// reopen it through engine-owned temporary work, restore the state, and run
// the ordinary canonical finalizer. The optimizer-selected physical node and
// exact MGA/security/recheck contract govern the route; temporary metadata
// never owns row visibility, transaction finality, or recovery.
static CanonicalAggregateStateSpillResult
ExecuteCanonicalAggregateStateSpillSelected(
    const CanonicalAggregateStateSpillRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
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

  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(
           request.aggregate_request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(
           request.aggregate_request.input_batch))) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-AUTHORITY-V1",
        "aggregate spill request carries conflicting owned execution carriers");
  }

  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority, execution_dag);
  if (!entry_authority.ok) {
    return refuse(entry_authority.diagnostic_code, entry_authority.detail);
  }

  auto baseline = ExecuteCanonicalAggregateRuntimeSelected(
      request.aggregate_request, execution_dag, execution_input_batch,
      borrowed_execution_carriers, false,
      {request.retained_memory_bytes, std::nullopt});
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
  for (const auto& node : execution_dag.nodes) {
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
      !execution_dag.spill_allowed) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-STRATEGY-V1",
                  "aggregate state spill was not selected and permitted by the physical plan");
  }
  const auto node_memory_grant = AggregateNodeMemoryGrant(
      execution_dag, *selected_node);
  if (request.retained_memory_bytes >
      std::numeric_limits<std::size_t>::max() -
          request.aggregate_request.retained_memory_bytes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate spill retained memory size overflowed");
  }
  const auto enclosing_retained_memory_bytes =
      request.retained_memory_bytes +
      request.aggregate_request.retained_memory_bytes;
  std::size_t filter_memory_bytes = 0;
  if ((request.aggregate_request.filter_truth_values.has_value() &&
       !CheckedAggregateFinalizationMultiply(
           request.aggregate_request.filter_truth_values->size(),
           sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
           &filter_memory_bytes)) ||
      filter_memory_bytes >
          std::numeric_limits<std::size_t>::max() -
              enclosing_retained_memory_bytes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate spill FILTER memory size overflowed");
  }
  const auto combined_retained_memory_bytes =
      enclosing_retained_memory_bytes + filter_memory_bytes;
  std::size_t input_payload_bytes = 0;
  if (!node_memory_grant.has_value() ||
      !AggregateBatchPayloadBytes(execution_input_batch,
                                  &input_payload_bytes) ||
      combined_retained_memory_bytes > *node_memory_grant ||
      input_payload_bytes >
          *node_memory_grant - combined_retained_memory_bytes ||
      baseline.final_output_bytes >
          *node_memory_grant - combined_retained_memory_bytes -
              input_payload_bytes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate spill selected-node memory grant is exhausted");
  }
  const auto replay_state_limit = std::min(
      request.aggregate_request.maximum_state_bytes,
      *node_memory_grant - combined_retained_memory_bytes -
          input_payload_bytes - baseline.final_output_bytes);
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
  if (!PrepareCanonicalAggregateTransitions(
          request.aggregate_request, execution_input_batch,
          replay_state_limit,
          &prepared, &state_diagnostic)) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  CanonicalAggregateCoreState state;
  if (prepared.retained_bytes > replay_state_limit ||
      !BuildCanonicalAggregateCoreState(
          request.aggregate_request, execution_input_batch,
          prepared.transitions,
          replay_state_limit - prepared.retained_bytes, &state,
          &state_diagnostic)) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  const auto serialized_source_state_bytes =
      EstimateCanonicalAggregateStateBytes(state);
  std::vector<std::size_t>().swap(prepared.transitions);
  prepared.retained_bytes = 0;
  if (serialized_source_state_bytes > replay_state_limit) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate spill source state exhausted the selected-node grant");
  }
  const auto live_serialized_state_limit = std::min(
      request.maximum_serialized_state_bytes,
      replay_state_limit - serialized_source_state_bytes);
  AggregateStateBytes serialized;
  if (!SerializeCanonicalAggregateCoreState(
          request.aggregate_request, state,
          live_serialized_state_limit, &serialized)) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SERIALIZE-V1",
                  "aggregate state cannot be canonically serialized within its bound");
  }
  result.state_serialized = true;
  result.serialized_state_bytes = serialized.size();
  const auto expected_serialized_state_bytes = serialized.size();
  state = {};
  constexpr std::size_t kBytesPerSpillRecord = 7;
  const auto record_count =
      serialized.size() / kBytesPerSpillRecord +
      (serialized.size() % kBytesPerSpillRecord != 0 ? 1U : 0U);
  if (record_count == 0 ||
      record_count > request.maximum_spill_record_count) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-RESOURCE-V1",
                  "serialized aggregate state exceeds its spill record bound");
  }
  constexpr std::size_t kSpillRecordKeyBytes =
      std::string_view("qow205.state.").size() + 20;
  constexpr std::size_t kMaximumSpillOutputRowBytes =
      kSpillRecordKeyBytes + 1 + 20 + 1 + 20;
  std::size_t live_spill_row_bytes = 0;
  std::size_t live_spill_input_row_bytes = 0;
  std::size_t input_record_bytes = sizeof(TempSpillInputRow) +
                                   kSpillRecordKeyBytes;
  std::size_t output_record_bytes = sizeof(std::string) +
                                    kMaximumSpillOutputRowBytes;
  if (!CheckedAggregateFinalizationMultiply(
          record_count, input_record_bytes,
          &live_spill_input_row_bytes) ||
      !CheckedAggregateFinalizationAdd(&input_record_bytes,
                                        output_record_bytes) ||
      !CheckedAggregateFinalizationMultiply(record_count,
                                              input_record_bytes,
                                              &live_spill_row_bytes) ||
      expected_serialized_state_bytes > replay_state_limit ||
      live_spill_input_row_bytes >
          replay_state_limit - expected_serialized_state_bytes ||
      live_spill_row_bytes > replay_state_limit) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate spill records exceed the selected-node grant");
  }

  TempSpillRequest spill;
  spill.route_kind = TempSpillRouteKind::kSort;
  spill.route_label =
      "qow205.aggregate-registry-state." + request.spill_owner_uuid;
  spill.spill_directory = owner_directory;
  spill.runtime_generation = request.runtime_generation;
  spill.reopen_runtime_generation = request.reopen_runtime_generation;
  spill.memory_quota_bytes = request.memory_quota_bytes;
  spill.memory_quota_bytes = std::min<std::uint64_t>(
      spill.memory_quota_bytes,
      replay_state_limit - live_spill_input_row_bytes);
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
    constexpr std::string_view prefix = "qow205.state.";
    std::string key(prefix);
    key.append(20, '0');
    std::array<char, 32> digits{};
    const auto rendered = std::to_chars(
        digits.data(), digits.data() + digits.size(), index);
    if (rendered.ec != std::errc{}) return std::string{};
    const auto digit_count = static_cast<std::size_t>(
        rendered.ptr - digits.data());
    if (digit_count > 20) return std::string{};
    std::copy(digits.begin(), digits.begin() +
                                  static_cast<std::ptrdiff_t>(digit_count),
              key.end() - static_cast<std::ptrdiff_t>(digit_count));
    return key;
  };
  for (std::size_t record = 0; record < record_count; ++record) {
    std::int64_t payload = 0;
    for (std::size_t byte = 0; byte < kBytesPerSpillRecord; ++byte) {
      const auto offset = record * kBytesPerSpillRecord + byte;
      if (offset == serialized.size()) break;
      payload |= static_cast<std::int64_t>(serialized[offset]) << (byte * 8);
    }
    auto key = record_key(record);
    if (key.size() != kSpillRecordKeyBytes) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "aggregate spill record identity overflowed");
    }
    spill.rows.push_back(
        {std::move(key), payload, static_cast<std::uint64_t>(record + 1)});
  }
  spill.expected_result_hash =
      ComputeOrderedTempSpillSortResultHash(spill.rows);
  if (spill.expected_result_hash.empty()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate spill expected result hash could not be bound");
  }
  spill.maximum_live_memory_bytes = replay_state_limit;
  spill.retained_memory_bytes = 0;
  AggregateStateBytes().swap(serialized);

  auto spilled = ExecuteBoundedTempSpillRoute(std::move(spill));
  result.spilled = spilled.spilled;
  result.spill_reopened = spilled.reopen_recovery_proven;
  result.cleanup_proven = spilled.cleanup_proven;
  result.cancellation_observed = request.cancellation_requested;
  result.spill_evidence = spilled.evidence;
  result.spilled_state_record_count = record_count;
  if (has_owned_artifact()) {
    result.cleanup_proven =
        remove_owned_artifacts() && result.cleanup_proven;
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-CLEANUP-V1",
                  "aggregate state spill artifact survived cleanup");
  }
  if (!spilled.ok || !spilled.spilled || !spilled.cleanup_proven ||
      !spilled.reopen_recovery_proven ||
      spilled.output_rows.size() != record_count ||
      spilled.peak_live_memory_bytes > replay_state_limit) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-SPILL-V1",
                  spilled.diagnostic_code + ":" + spilled.fallback_reason);
  }

  AggregateStateBytes reopened_bytes;
  if (expected_serialized_state_bytes >
      replay_state_limit - live_spill_row_bytes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate reopened state exceeds the selected-node grant");
  }
  reopened_bytes.reserve(expected_serialized_state_bytes);
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
         reopened_bytes.size() < expected_serialized_state_bytes;
         ++byte) {
      reopened_bytes.push_back(
          static_cast<std::uint8_t>(payload >> (byte * 8)));
    }
  }
  if (reopened_bytes.size() != expected_serialized_state_bytes) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-RESTORE-V1",
                  "reopened aggregate state byte count differs from the serialized state");
  }

  if (reopened_bytes.size() > replay_state_limit ||
      baseline.state_bytes > replay_state_limit - reopened_bytes.size()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate restored state exceeds the selected-node grant");
  }
  decltype(spill.rows)().swap(spill.rows);
  decltype(spilled.output_rows)().swap(spilled.output_rows);
  CanonicalAggregateCoreState restored_state;
  if (reopened_bytes.size() > replay_state_limit) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "reopened aggregate state exhausted the selected-node grant");
  }
  if (!DeserializeCanonicalAggregateCoreState(
          request.aggregate_request, reopened_bytes,
          request.maximum_serialized_state_bytes,
          replay_state_limit - reopened_bytes.size(), &restored_state)) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-RESTORE-V1",
                  "reopened aggregate state cannot be decoded");
  }
  result.state_restored = true;
  const auto restored_state_bytes =
      EstimateCanonicalAggregateStateBytes(restored_state);
  state = CanonicalAggregateCoreState{};
  std::vector<std::size_t>().swap(prepared.transitions);
  AggregateStateBytes().swap(serialized);
  AggregateStateBytes().swap(reopened_bytes);
  if (combined_retained_memory_bytes > *node_memory_grant ||
      input_payload_bytes >
          *node_memory_grant - combined_retained_memory_bytes ||
      baseline.final_output_bytes >
          *node_memory_grant - combined_retained_memory_bytes -
              input_payload_bytes ||
      restored_state_bytes >
          *node_memory_grant - combined_retained_memory_bytes -
              input_payload_bytes - baseline.final_output_bytes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate spill replay exhausted the selected-node memory grant");
  }
  const auto live_finalization_bytes =
      *node_memory_grant - combined_retained_memory_bytes -
      input_payload_bytes - baseline.final_output_bytes -
      restored_state_bytes;
  const auto maximum_final_output_bytes =
      request.aggregate_request.maximum_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(
                request.aggregate_request.maximum_final_output_bytes,
                *node_memory_grant);
  const auto maximum_finalization_workspace_bytes =
      request.aggregate_request.maximum_finalization_workspace_bytes == 0
          ? *node_memory_grant
          : std::min(
                request.aggregate_request
                    .maximum_finalization_workspace_bytes,
                *node_memory_grant);
  state_diagnostic = {};
  CanonicalAggregateFinalizationReceipt finalization_receipt;
  auto restored_value = FinalizeCanonicalAggregateCore(
      request.aggregate_request, restored_state, &state_diagnostic,
      &finalization_receipt, maximum_final_output_bytes,
      maximum_finalization_workspace_bytes, live_finalization_bytes);
  if (!state_diagnostic.ok) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  DescriptorBatch restored_output;
  restored_output.columns = {request.aggregate_request.result_column};
  restored_output.rows = {{{std::move(restored_value)}}};
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      restored_output,
      {request.aggregate_request.result_column.descriptor_id});
  if (!output_validation.ok ||
      !SameAggregateValueIdentity(
          baseline.output_batch.rows.front().values.front(),
          restored_output.rows.front().values.front()) ||
      baseline.transition_count != restored_state.transition_count ||
      baseline.non_null_transition_count != restored_state.non_null_count ||
      baseline.state_bytes != restored_state_bytes ||
      baseline.final_output_bytes != finalization_receipt.output_bytes ||
      baseline.peak_finalization_workspace_bytes !=
          finalization_receipt.peak_workspace_bytes) {
    return refuse("QOW-DIAG-QRY-011-REGISTRY-STATE-EQUIVALENCE-V1",
                  "restored aggregate state finalization differs from the in-memory route");
  }
  auto restored = std::move(baseline);
  restored.output_batch = std::move(restored_output);
  restored.transition_count = restored_state.transition_count;
  restored.non_null_transition_count = restored_state.non_null_count;
  restored.state_bytes = restored_state_bytes;
  restored.final_output_bytes = finalization_receipt.output_bytes;
  restored.peak_finalization_workspace_bytes =
      finalization_receipt.peak_workspace_bytes;
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority, execution_dag);
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

CanonicalAggregateStateSpillResult ExecuteCanonicalAggregateStateSpill(
    const CanonicalAggregateStateSpillRequest& request) {
  return ExecuteCanonicalAggregateStateSpillSelected(
      request, request.aggregate_request.physical_dag,
      request.aggregate_request.input_batch, false);
}

CanonicalAggregateStateSpillResult ExecuteCanonicalAggregateStateSpill(
    const CanonicalAggregateStateSpillRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalAggregateStateSpillSelected(
      request, borrowed_execution_dag, borrowed_input_batch, true);
}

// QOW-SOURCE-QRY-011-REGISTRY-STATE-EXCHANGE-V1
// Materialize one complete transition state per optimizer-bound worker,
// serialize each state across an explicit exchange boundary, restore it at
// the coordinator, and merge in worker-ordinal order through the same state
// authority used by serial, local-combine, spill, grouped, and window routes.
// The exchange carries no transaction-finality, recovery, visibility, or
// parser-execution authority of its own.
static CanonicalAggregateStateExchangeResult
ExecuteCanonicalAggregateStateExchangeSelected(
    const CanonicalAggregateStateExchangeRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
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

  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(
           request.aggregate_request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(
           request.aggregate_request.input_batch))) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-AUTHORITY-V1",
        "aggregate exchange request carries conflicting owned execution carriers");
  }

  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority, execution_dag);
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

  auto baseline = ExecuteCanonicalAggregateRuntimeSelected(
      request.aggregate_request, execution_dag, execution_input_batch,
      borrowed_execution_carriers, true, {});
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

  const auto selected_node = std::ranges::find_if(
      execution_dag.nodes,
      [&](const auto& node) {
        return node.physical_node_id ==
               request.aggregate_request.selected_physical_node_id;
      });
  std::size_t input_payload_bytes = 0;
  std::size_t filter_memory_bytes = 0;
  std::size_t transition_ordinal_memory_bytes = 0;
  if (selected_node == execution_dag.nodes.end() ||
      !AggregateBatchPayloadBytes(execution_input_batch,
                                  &input_payload_bytes) ||
      (request.aggregate_request.filter_truth_values.has_value() &&
       !CheckedAggregateFinalizationMultiply(
           request.aggregate_request.filter_truth_values->size(),
           sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
           &filter_memory_bytes)) ||
      !CheckedAggregateFinalizationMultiply(
          baseline.transition_row_indices.capacity(), sizeof(std::size_t),
          &transition_ordinal_memory_bytes)) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate exchange selected-node grant is unresolved");
  }
  const auto node_memory_grant = AggregateNodeMemoryGrant(
      execution_dag, *selected_node);
  if (!node_memory_grant.has_value() ||
      request.aggregate_request.retained_memory_bytes >
          *node_memory_grant ||
      input_payload_bytes >
          *node_memory_grant -
              request.aggregate_request.retained_memory_bytes ||
      filter_memory_bytes >
          *node_memory_grant -
              request.aggregate_request.retained_memory_bytes -
              input_payload_bytes ||
      baseline.final_output_bytes >
          *node_memory_grant -
              request.aggregate_request.retained_memory_bytes -
              input_payload_bytes - filter_memory_bytes ||
      transition_ordinal_memory_bytes >
          *node_memory_grant -
              request.aggregate_request.retained_memory_bytes -
              input_payload_bytes - filter_memory_bytes -
              baseline.final_output_bytes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "aggregate exchange selected-node grant is exhausted");
  }
  const auto exchange_fixed_memory =
      request.aggregate_request.retained_memory_bytes +
      input_payload_bytes + filter_memory_bytes + baseline.final_output_bytes +
      transition_ordinal_memory_bytes;
  DescriptorRuntimeDiagnostic state_diagnostic;
  const auto maximum_state_bytes = std::min(
      request.aggregate_request.maximum_state_bytes,
      *node_memory_grant - exchange_fixed_memory);

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

  const auto& canonical_transitions = baseline.transition_row_indices;
  const auto base_partition_size = canonical_transitions.size() / worker_count;
  const auto extra_partition_count = canonical_transitions.size() % worker_count;
  std::size_t partition_begin = 0;
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    const auto partition_size =
        base_partition_size + (worker < extra_partition_count ? 1 : 0);
    const auto partition_end = partition_begin + partition_size;
    CanonicalAggregateCoreState partial_state;
    const auto live_partial_capacity_limit =
        *node_memory_grant - exchange_fixed_memory -
        result.serialized_state_bytes;
    if (!ReserveCanonicalAggregateCoreState(
            request.aggregate_request.descriptor, partition_size,
            std::min(maximum_state_bytes, live_partial_capacity_limit),
            &partial_state, &state_diagnostic)) {
      return refuse(state_diagnostic.diagnostic_code,
                    state_diagnostic.detail);
    }
    for (std::size_t transition = partition_begin;
         transition < partition_end; ++transition) {
      const auto live_partial_state_limit =
          *node_memory_grant - exchange_fixed_memory -
          result.serialized_state_bytes;
      if (!TransitionCanonicalAggregateCoreBounded(
              request.aggregate_request.descriptor,
              BindAggregateTransitionValues(
                  request.aggregate_request, execution_input_batch,
                  canonical_transitions[transition]),
              live_partial_state_limit, &partial_state,
              &state_diagnostic)) {
        return refuse(state_diagnostic.diagnostic_code,
                      state_diagnostic.detail);
      }
      if (EstimateCanonicalAggregateStateBytes(partial_state) >
          maximum_state_bytes) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "aggregate exchange partial-state byte bound is exceeded");
      }
    }
    partition_begin = partition_end;

    AggregateStateBytes serialized;
    const auto partial_state_bytes =
        EstimateCanonicalAggregateStateBytes(partial_state);
    if (result.serialized_state_bytes >
            *node_memory_grant - exchange_fixed_memory ||
        partial_state_bytes >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes) {
      return refuse(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "aggregate exchange worker state exhausted the selected-node grant");
    }
    const auto live_serialized_state_limit = std::min(
        request.maximum_serialized_state_bytes_per_worker,
        *node_memory_grant - exchange_fixed_memory -
            result.serialized_state_bytes -
            partial_state_bytes);
    if (!SerializeCanonicalAggregateCoreState(
            request.aggregate_request, partial_state,
            live_serialized_state_limit,
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
    if (result.serialized_state_bytes >
            *node_memory_grant - exchange_fixed_memory ||
        serialized.size() >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes ||
        partial_state_bytes >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes - serialized.size()) {
      return refuse(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "aggregate exchange live worker state exceeds the selected-node grant");
    }
    result.serialized_state_bytes += serialized.size();
    result.worker_transition_counts.push_back(partial_state.transition_count);
    result.worker_state_bytes.push_back(
        partial_state_bytes);
    result.worker_serialized_state_bytes.push_back(serialized.size());
    exchanged.push_back({request.exchange_generation,
                         request.worker_ordinals[worker],
                         request.aggregate_request.descriptor.function,
                         std::move(serialized)});
  }
  if (partition_begin != canonical_transitions.size()) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-PARTITION-V1",
        "aggregate exchange partitions do not cover the prepared transitions");
  }
  result.partial_state_count = exchanged.size();
  result.states_serialized = true;

  CanonicalAggregateCoreState merged_state;
  const auto merged_capacity_limit = std::min(
      maximum_state_bytes,
      *node_memory_grant - exchange_fixed_memory -
          result.serialized_state_bytes);
  if (!ReserveCanonicalAggregateCoreState(
          request.aggregate_request.descriptor,
          result.worker_transition_counts.empty()
              ? 0
              : std::accumulate(result.worker_transition_counts.begin(),
                                result.worker_transition_counts.end(),
                                std::size_t{0}),
          merged_capacity_limit, &merged_state, &state_diagnostic)) {
    return refuse(state_diagnostic.diagnostic_code,
                  state_diagnostic.detail);
  }
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
    const auto prior_merged_state_bytes =
        EstimateCanonicalAggregateStateBytes(merged_state);
    if (result.serialized_state_bytes >
            *node_memory_grant - exchange_fixed_memory ||
        prior_merged_state_bytes >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "aggregate exchange restore memory is exhausted");
    }
    const auto restore_live_state_limit =
        *node_memory_grant - exchange_fixed_memory -
        result.serialized_state_bytes - prior_merged_state_bytes;
    if (!DeserializeCanonicalAggregateCoreState(
            request.aggregate_request, envelope.bytes,
            request.maximum_serialized_state_bytes_per_worker,
            restore_live_state_limit, &restored_state) ||
        restored_state.transition_count !=
            result.worker_transition_counts[worker] ||
        EstimateCanonicalAggregateStateBytes(restored_state) !=
            result.worker_state_bytes[worker]) {
      return refuse(
          "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-RESTORE-V1",
          "aggregate partial state cannot be restored exactly");
    }
    ++result.restored_partial_state_count;
    const auto restored_state_bytes =
        EstimateCanonicalAggregateStateBytes(restored_state);
    if (result.serialized_state_bytes >
            *node_memory_grant - exchange_fixed_memory ||
        restored_state_bytes >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes ||
        prior_merged_state_bytes >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes - restored_state_bytes) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "aggregate exchange merge inputs exceed the selected-node grant");
    }
    const auto merge_live_state_limit =
        *node_memory_grant - exchange_fixed_memory -
        result.serialized_state_bytes;
    if (!MergeCanonicalAggregateCoreBounded(
            request.aggregate_request.descriptor, &merged_state,
            std::move(restored_state), merge_live_state_limit,
            &state_diagnostic)) {
      return refuse(state_diagnostic.diagnostic_code,
                    state_diagnostic.detail);
    }
    const auto merged_state_bytes =
        EstimateCanonicalAggregateStateBytes(merged_state);
    if (merged_state_bytes > maximum_state_bytes ||
        result.serialized_state_bytes >
            *node_memory_grant - exchange_fixed_memory ||
        restored_state_bytes >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes ||
        merged_state_bytes >
            *node_memory_grant - exchange_fixed_memory -
                result.serialized_state_bytes - restored_state_bytes) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "aggregate exchange merged-state memory bound is exceeded");
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
  const auto merged_state_bytes =
      EstimateCanonicalAggregateStateBytes(merged_state);
  decltype(exchanged)().swap(exchanged);
  if (merged_state_bytes >
      *node_memory_grant - exchange_fixed_memory) {
    return refuse(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate exchange finalization exhausted the selected-node grant");
  }
  const auto maximum_final_output_bytes =
      request.aggregate_request.maximum_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(
                request.aggregate_request.maximum_final_output_bytes,
                *node_memory_grant);
  const auto maximum_finalization_workspace_bytes =
      request.aggregate_request.maximum_finalization_workspace_bytes == 0
          ? *node_memory_grant
          : std::min(
                request.aggregate_request
                    .maximum_finalization_workspace_bytes,
                *node_memory_grant);
  CanonicalAggregateFinalizationReceipt finalization_receipt;
  auto merged_value = FinalizeCanonicalAggregateCore(
      request.aggregate_request, merged_state, &state_diagnostic,
      &finalization_receipt, maximum_final_output_bytes,
      maximum_finalization_workspace_bytes,
      *node_memory_grant - exchange_fixed_memory - merged_state_bytes);
  if (!state_diagnostic.ok) {
    return refuse(state_diagnostic.diagnostic_code, state_diagnostic.detail);
  }
  DescriptorBatch merged_output;
  merged_output.columns = {request.aggregate_request.result_column};
  merged_output.rows = {{{std::move(merged_value)}}};
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      merged_output,
      {request.aggregate_request.result_column.descriptor_id});
  if (!output_validation.ok ||
      !SameAggregateValueIdentity(
          baseline.output_batch.rows.front().values.front(),
          merged_output.rows.front().values.front()) ||
      baseline.transition_count != merged_state.transition_count ||
      baseline.non_null_transition_count != merged_state.non_null_count ||
      baseline.state_bytes != merged_state_bytes ||
      baseline.final_output_bytes != finalization_receipt.output_bytes ||
      baseline.peak_finalization_workspace_bytes !=
          finalization_receipt.peak_workspace_bytes) {
    return refuse(
        "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-EQUIVALENCE-V1",
        "merged aggregate exchange finalization differs from local combine");
  }
  auto merged = std::move(baseline);
  merged.output_batch = std::move(merged_output);
  merged.transition_count = merged_state.transition_count;
  merged.non_null_transition_count = merged_state.non_null_count;
  merged.state_bytes = merged_state_bytes;
  merged.final_output_bytes = finalization_receipt.output_bytes;
  merged.peak_finalization_workspace_bytes =
      finalization_receipt.peak_workspace_bytes;
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.aggregate_request.mga_authority, execution_dag);
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

CanonicalAggregateStateExchangeResult ExecuteCanonicalAggregateStateExchange(
    const CanonicalAggregateStateExchangeRequest& request) {
  return ExecuteCanonicalAggregateStateExchangeSelected(
      request, request.aggregate_request.physical_dag,
      request.aggregate_request.input_batch, false);
}

CanonicalAggregateStateExchangeResult ExecuteCanonicalAggregateStateExchange(
    const CanonicalAggregateStateExchangeRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalAggregateStateExchangeSelected(
      request, borrowed_execution_dag, borrowed_input_batch, true);
}

CanonicalAggregateRuntimeRequest CloneCanonicalAggregateRequestWithoutRows(
    const CanonicalAggregateRuntimeRequest& source,
    bool copy_execution_carriers = true);

// QOW-SOURCE-QRY-011-REGISTRY-MOVING-INVERSE-V1
// Maintain one exact COUNT/SUM/AVG state across an ordered sequence of window
// frames.  Additions use the canonical transition function, removals use an
// explicitly admitted inverse transition, and every output uses the same
// canonical finalizer.  Unsupported functions and modifiers fail closed
// rather than relabelling frame recomputation as inverse execution.
static CanonicalAggregateMovingRuntimeResult
ExecuteCanonicalAggregateMovingRuntimeSelected(
    const CanonicalAggregateMovingRuntimeRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  using scratchbird::engine::internal_api::EnginePredicateConsumer;
  using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;

  CanonicalAggregateMovingRuntimeResult result;
  result.descriptor = request.aggregate_request.descriptor;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    const auto descriptor = result.descriptor;
    result = {};
    result.descriptor = descriptor;
    result.diagnostic = std::move(diagnostic);
    result.transient_state_cleanup_proven = true;
    result.all_or_nothing_publication = true;
    return result;
  };
  const auto& aggregate = request.aggregate_request;
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(aggregate.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(aggregate.input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-INVERSE-AUTHORITY-V1",
        "moving aggregate request carries conflicting owned execution carriers"));
  }
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, execution_dag);
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
  if (request.cancellation_requested) {
    auto cancelled = refuse(Refusal(
        "QOW-DIAG-QRY-011-REGISTRY-INVERSE-CANCELLED-V1",
        "moving aggregate execution was cancelled before publication"));
    cancelled.cancellation_observed = true;
    return cancelled;
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
  if (aggregate.descriptor.function == CanonicalAggregateFunction::sum ||
      aggregate.descriptor.function == CanonicalAggregateFunction::avg) {
    if (aggregate.value_columns.size() != 1 ||
        aggregate.value_columns.front() >= execution_input_batch.columns.size()) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-011-REGISTRY-INVERSE-TYPE-V1",
          "moving SUM and AVG inverse state requires one numeric input"));
    }
    const auto& value_descriptor =
        execution_input_batch.columns[aggregate.value_columns.front()]
            .descriptor;
    if (!IsCanonicalBoundedSignedIntegerDescriptor(value_descriptor) &&
        value_descriptor.canonical_type_name != "real64") {
      return refuse(Refusal(
          "QOW-DIAG-QRY-011-REGISTRY-INVERSE-TYPE-V1",
          "moving SUM and AVG inverse state requires bounded signed integer "
          "or real64 input"));
    }
  }
  if (aggregate.filter_truth_values.has_value() &&
      aggregate.filter_truth_values->size() != execution_input_batch.rows.size()) {
    return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                          "moving aggregate FILTER cardinality is not bound"));
  }
  std::vector<std::uint32_t> input_descriptor_ids;
  input_descriptor_ids.reserve(execution_input_batch.columns.size());
  for (const auto& column : execution_input_batch.columns) {
    input_descriptor_ids.push_back(column.descriptor_id);
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      execution_input_batch, input_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  const auto aggregate_node = std::ranges::find_if(
      execution_dag.nodes, [&](const auto& node) {
        return node.physical_node_id == aggregate.selected_physical_node_id;
      });
  std::size_t input_payload_bytes = 0;
  std::size_t filter_memory_bytes = 0;
  if (aggregate_node == execution_dag.nodes.end() ||
      !AggregateBatchPayloadBytes(execution_input_batch,
                                  &input_payload_bytes) ||
      (aggregate.filter_truth_values.has_value() &&
       !CheckedAggregateFinalizationMultiply(
           aggregate.filter_truth_values->size(),
           sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
           &filter_memory_bytes))) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "moving aggregate selected-node memory grant is unresolved"));
  }
  const auto node_memory_grant =
      AggregateNodeMemoryGrant(execution_dag, *aggregate_node);
  if (!node_memory_grant.has_value() ||
      aggregate.retained_memory_bytes > *node_memory_grant ||
      input_payload_bytes >
          *node_memory_grant - aggregate.retained_memory_bytes ||
      filter_memory_bytes >
          *node_memory_grant - aggregate.retained_memory_bytes -
              input_payload_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "moving aggregate input exhausts the selected-node memory grant"));
  }
  const auto fixed_input_memory =
      aggregate.retained_memory_bytes + input_payload_bytes +
      filter_memory_bytes;
  const auto maximum_combined_final_output_bytes =
      request.maximum_combined_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(request.maximum_combined_final_output_bytes,
                     *node_memory_grant);
  const auto maximum_final_output_bytes =
      aggregate.maximum_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(aggregate.maximum_final_output_bytes,
                     *node_memory_grant);
  const auto maximum_finalization_workspace_bytes =
      aggregate.maximum_finalization_workspace_bytes == 0
          ? *node_memory_grant
          : std::min(aggregate.maximum_finalization_workspace_bytes,
                     *node_memory_grant);
  for (const auto& frame : request.effective_frame_row_indices) {
    if (!std::is_sorted(frame.begin(), frame.end()) ||
        std::adjacent_find(frame.begin(), frame.end()) != frame.end() ||
        (!frame.empty() && frame.back() >= execution_input_batch.rows.size())) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-011-REGISTRY-INVERSE-FRAME-V1",
          "moving aggregate frame rows are not sorted unique input handles"));
    }
  }

  auto preflight_request =
      CloneCanonicalAggregateRequestWithoutRows(aggregate, false);
  DescriptorBatch preflight_input_batch;
  preflight_input_batch.columns = execution_input_batch.columns;
  auto preflight = ExecuteCanonicalAggregateRuntimeSelected(
      preflight_request, execution_dag, preflight_input_batch, true, false,
      {input_payload_bytes - 1 + filter_memory_bytes, std::nullopt});
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
  result.peak_finalization_workspace_bytes =
      preflight.peak_finalization_workspace_bytes;
  preflight.output_batch = {};

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
    return BindAggregateTransitionValues(
        aggregate, execution_input_batch, row);
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
    if (result.combined_final_output_bytes >
        *node_memory_grant - fixed_input_memory) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "moving aggregate retained output exhausted the selected-node grant"));
    }
    const auto live_transition_state_limit = std::min(
        aggregate.maximum_state_bytes,
        *node_memory_grant - fixed_input_memory -
            result.combined_final_output_bytes);
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
      if (!TransitionCanonicalAggregateCoreBounded(
              aggregate.descriptor, transition_values(row),
              live_transition_state_limit, &state, &state_diagnostic)) {
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
    const auto remaining_finalization_bytes =
        maximum_combined_final_output_bytes -
        result.combined_final_output_bytes;
    const auto bounded_final_output_bytes = std::min(
        maximum_final_output_bytes, remaining_finalization_bytes);
    if (fixed_input_memory > *node_memory_grant ||
        state_bytes > *node_memory_grant - fixed_input_memory ||
        result.combined_final_output_bytes >
            *node_memory_grant - fixed_input_memory - state_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "moving aggregate live memory exhausted the selected-node grant"));
    }
    const auto live_finalization_bytes =
        *node_memory_grant - fixed_input_memory - state_bytes -
        result.combined_final_output_bytes;
    CanonicalAggregateFinalizationReceipt finalization_receipt;
    auto value = FinalizeCanonicalAggregateCore(
        aggregate, state, &state_diagnostic, &finalization_receipt,
        bounded_final_output_bytes,
        maximum_finalization_workspace_bytes,
        live_finalization_bytes);
    if (!state_diagnostic.ok) return refuse(std::move(state_diagnostic));
    if (!within_total(finalization_receipt.output_bytes,
                      result.combined_final_output_bytes,
                      maximum_combined_final_output_bytes)) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "moving aggregate final output byte bound is exceeded"));
    }
    result.combined_final_output_bytes +=
        finalization_receipt.output_bytes;
    result.peak_finalization_workspace_bytes = std::max(
        result.peak_finalization_workspace_bytes,
        finalization_receipt.peak_workspace_bytes);
    result.values.push_back(std::move(value));
    active_frame = frame;
  }

  DescriptorBatch output;
  output.columns = {aggregate.result_column};
  output.rows.reserve(result.values.size());
  for (auto& value : result.values) {
    output.rows.push_back({{std::move(value)}});
  }
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {aggregate.result_column.descriptor_id});
  if (!output_validation.ok) return refuse(output_validation);
  result.values.clear();
  result.values.reserve(output.rows.size());
  for (auto& row : output.rows) {
    result.values.push_back(std::move(row.values.front()));
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.moving_inverse_state_used = true;
  result.frame_recomputation_used = false;
  result.transient_state_cleanup_proven = true;
  result.all_or_nothing_publication = true;
  result.authority = preflight.authority;
  result.selected_plan_uuid = preflight.selected_plan_uuid;
  result.executed_physical_node_id = preflight.executed_physical_node_id;
  result.causal_counter_id = preflight.causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

CanonicalAggregateMovingRuntimeResult ExecuteCanonicalAggregateMovingRuntime(
    const CanonicalAggregateMovingRuntimeRequest& request) {
  return ExecuteCanonicalAggregateMovingRuntimeSelected(
      request, request.aggregate_request.physical_dag,
      request.aggregate_request.input_batch, false);
}

CanonicalAggregateMovingRuntimeResult ExecuteCanonicalAggregateMovingRuntime(
    const CanonicalAggregateMovingRuntimeRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalAggregateMovingRuntimeSelected(
      request, borrowed_execution_dag, borrowed_input_batch, true);
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

CanonicalAggregateRuntimeRequest CloneCanonicalAggregateRequestWithoutRows(
    const CanonicalAggregateRuntimeRequest& source,
    const bool copy_execution_carriers) {
  CanonicalAggregateRuntimeRequest clone;
  if (copy_execution_carriers) {
    clone.physical_dag = source.physical_dag;
    clone.input_batch.columns = source.input_batch.columns;
  }
  clone.selected_physical_node_id = source.selected_physical_node_id;
  clone.descriptor = source.descriptor;
  clone.value_columns = source.value_columns;
  clone.value_expression_descriptor_ids =
      source.value_expression_descriptor_ids;
  clone.direct_arguments = source.direct_arguments;
  clone.result_column = source.result_column;
  if (source.filter_truth_values.has_value()) {
    clone.filter_truth_values =
        std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>{};
  }
  clone.distinct = source.distinct;
  clone.aggregate_order_terms = source.aggregate_order_terms;
  clone.aggregate_separator = source.aggregate_separator;
  clone.listagg_overflow_mode = source.listagg_overflow_mode;
  clone.listagg_max_output_bytes = source.listagg_max_output_bytes;
  clone.listagg_truncation_indicator = source.listagg_truncation_indicator;
  clone.listagg_with_count = source.listagg_with_count;
  clone.forced_strategy = source.forced_strategy;
  clone.maximum_transition_count = source.maximum_transition_count;
  clone.maximum_distinct_value_count = source.maximum_distinct_value_count;
  clone.maximum_aggregate_order_term_count =
      source.maximum_aggregate_order_term_count;
  clone.maximum_order_comparison_count =
      source.maximum_order_comparison_count;
  clone.maximum_state_bytes = source.maximum_state_bytes;
  clone.maximum_final_output_bytes = source.maximum_final_output_bytes;
  clone.maximum_finalization_workspace_bytes =
      source.maximum_finalization_workspace_bytes;
  clone.retained_memory_bytes = source.retained_memory_bytes;
  clone.parser_execution_authority_claimed =
      source.parser_execution_authority_claimed;
  clone.transaction_finality_claimed = source.transaction_finality_claimed;
  clone.recovery_authority_claimed = source.recovery_authority_claimed;
  clone.mga_authority = source.mga_authority;
  return clone;
}

// QOW-SOURCE-QRY-011-GROUPED-REGISTRY-V1
// Build explicit grouping sets over ordered, descriptor-bound keys and route
// every resulting group through the one canonical aggregate registry/state
// authority above.  Omitted grouping keys are published as typed SQL NULL and
// remain distinguishable from data NULL through GROUPING/GROUPING_ID metadata.
static CanonicalGroupedAggregateRuntimeResult
ExecuteCanonicalGroupedAggregateRuntimeSelected(
    const CanonicalGroupedAggregateRuntimeRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers,
    const bool spill_execution_context,
    const std::size_t retained_memory_bytes = 0,
    const std::optional<std::size_t> maximum_combined_output_override =
        std::nullopt,
    const std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>*
        shared_filter_truth_values = nullptr) {
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
    result.combined_final_output_bytes = 0;
    result.peak_finalization_workspace_bytes = 0;
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
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(aggregate.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(aggregate.input_batch))) {
    return grouped_refusal(
        "grouped aggregate request carries conflicting owned execution carriers");
  }
  const auto& input_batch = execution_input_batch;
  const auto* filter_truth_values =
      shared_filter_truth_values != nullptr
          ? shared_filter_truth_values
          : (aggregate.filter_truth_values.has_value()
                 ? &*aggregate.filter_truth_values
                 : nullptr);
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
      aggregate.mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  if (aggregate.selected_physical_node_id == 0 ||
      aggregate.selected_physical_node_id !=
          execution_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected grouped aggregate node is not the root"));
  }
  const PhysicalNodeRecord* aggregate_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
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
  for (const auto& node : execution_dag.nodes) {
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
      input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  const auto node_memory_grant =
      AggregateNodeMemoryGrant(execution_dag, *aggregate_node);
  std::size_t input_payload_bytes = 0;
  std::size_t source_filter_bytes = 0;
  if (!node_memory_grant.has_value() ||
      !AggregateBatchPayloadBytes(input_batch,
                                  &input_payload_bytes) ||
      (filter_truth_values != nullptr &&
       !CheckedAggregateFinalizationMultiply(
           filter_truth_values->size(),
           sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
           &source_filter_bytes)) ||
      retained_memory_bytes > *node_memory_grant ||
      aggregate.retained_memory_bytes >
          *node_memory_grant - retained_memory_bytes ||
      input_payload_bytes >
          *node_memory_grant - retained_memory_bytes -
              aggregate.retained_memory_bytes ||
      source_filter_bytes >
          *node_memory_grant - retained_memory_bytes -
              aggregate.retained_memory_bytes - input_payload_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "grouped aggregate selected-node memory grant is invalid or exhausted"));
  }
  auto maximum_combined_final_output_bytes =
      request.maximum_combined_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(request.maximum_combined_final_output_bytes,
                     *node_memory_grant);
  if (maximum_combined_output_override.has_value()) {
    maximum_combined_final_output_bytes = std::min(
        maximum_combined_final_output_bytes,
        *maximum_combined_output_override);
  }

  std::set<std::uint32_t> group_expression_ids;
  std::vector<bool> key_can_be_omitted(request.group_key_terms.size(), false);
  std::vector<CanonicalDescriptorOrderTerm> group_result_terms;
  group_result_terms.reserve(request.group_key_terms.size());
  for (std::size_t index = 0; index < request.group_key_terms.size(); ++index) {
    const auto& term = request.group_key_terms[index];
    if (term.column >= input_batch.columns.size()) {
      return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                            "group key column is unresolved"));
    }
    auto term_validation = ValidateCanonicalDescriptorOrderTerm(
        term, input_batch.columns[term.column]);
    if (!term_validation.ok) return refuse(std::move(term_validation));
    if (!group_expression_ids.insert(term.expression_descriptor_id).second) {
      return grouped_refusal("group key expression handles are not unique");
    }
    const auto& output = request.group_result_columns[index];
    if (output.descriptor_id == 0 ||
        aggregate_node->output_descriptor_ids[index] != output.descriptor_id ||
        output.descriptor.descriptor_kind !=
            input_batch.columns[term.column]
                .descriptor.descriptor_kind ||
        output.descriptor.canonical_type_name !=
            input_batch.columns[term.column]
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
    const auto& input_column = input_batch.columns[
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

  const auto row_count = input_batch.rows.size();
  if (filter_truth_values != nullptr &&
      filter_truth_values->size() != row_count) {
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

  // The grouped node's complete output was validated above. Its internal
  // registry-state kernel publishes only the aggregate field, so derive that
  // physical view once and borrow it across synchronous preflight/group calls.
  auto kernel_dag = execution_dag;
  for (auto& node : kernel_dag.nodes) {
    if (node.physical_node_id == aggregate.selected_physical_node_id) {
      node.output_descriptor_ids = {aggregate.result_column.descriptor_id};
      break;
    }
  }
  const auto make_kernel_request = [&]() {
    return CloneCanonicalAggregateRequestWithoutRows(aggregate, false);
  };
  auto preflight_request = make_kernel_request();
  DescriptorBatch preflight_input_batch;
  preflight_input_batch.columns = input_batch.columns;
  if (preflight_request.filter_truth_values.has_value()) {
    preflight_request.filter_truth_values =
        std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>{};
  }
  auto preflight = ExecuteCanonicalAggregateRuntimeSelected(
      preflight_request, kernel_dag, preflight_input_batch, true, false,
      {retained_memory_bytes + input_payload_bytes + source_filter_bytes,
       std::nullopt});
  if (!preflight.diagnostic.ok) {
    return refuse(preflight.diagnostic);
  }
  result.peak_finalization_workspace_bytes =
      preflight.peak_finalization_workspace_bytes;
  preflight.output_batch = {};

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
              input_batch.rows[row].values[term.column],
              input_batch
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
            input_batch.rows[row].values[term.column],
            input_batch.rows[row].values[term.column], term,
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
  std::size_t retained_output_payload_bytes = 0;
  for (const auto& group : working_groups) {
    auto group_request = make_kernel_request();
    DescriptorBatch group_input_batch;
    group_input_batch.columns = input_batch.columns;
    const auto remaining_finalization_bytes =
        maximum_combined_final_output_bytes -
        result.combined_final_output_bytes;
    const auto copy_fixed_memory =
        retained_memory_bytes + aggregate.retained_memory_bytes +
        input_payload_bytes + source_filter_bytes;
    std::size_t group_input_payload_bytes = 1;
    for (const auto row : group.source_rows) {
      for (const auto& value : input_batch.rows[row].values) {
        if (!CheckedAggregateFinalizationAdd(
                &group_input_payload_bytes,
                value.encoded_value.size()) ||
            !CheckedAggregateFinalizationAdd(
                &group_input_payload_bytes,
                value.binary_value.size())) {
          return refuse(Refusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "grouped aggregate input payload size overflowed"));
        }
      }
    }
    std::size_t group_filter_bytes = 0;
    if (filter_truth_values != nullptr &&
        !CheckedAggregateFinalizationMultiply(
            group.source_rows.size(),
            sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
            &group_filter_bytes)) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "grouped aggregate FILTER workspace size overflowed"));
    }
    if (retained_output_payload_bytes >
            *node_memory_grant - copy_fixed_memory ||
        group_input_payload_bytes >
            *node_memory_grant - copy_fixed_memory -
                retained_output_payload_bytes ||
        group_filter_bytes >
            *node_memory_grant - copy_fixed_memory -
                retained_output_payload_bytes - group_input_payload_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "grouped aggregate input or FILTER copy exceeds the selected-node grant"));
    }
    group_input_batch.rows.reserve(group.source_rows.size());
    for (const auto row : group.source_rows) {
      group_input_batch.rows.push_back(input_batch.rows[row]);
    }
    if (filter_truth_values != nullptr) {
      std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
          group_filter;
      group_filter.reserve(group.source_rows.size());
      for (const auto row : group.source_rows) {
        group_filter.push_back((*filter_truth_values)[row]);
      }
      group_request.filter_truth_values = std::move(group_filter);
    }
    const auto scope_retained_memory =
        retained_memory_bytes + input_payload_bytes + source_filter_bytes;
    const auto fixed_retained_memory =
        scope_retained_memory + aggregate.retained_memory_bytes;
    std::size_t group_key_payload_bytes = 0;
    for (std::size_t key_index = 0;
         key_index < request.group_result_columns.size(); ++key_index) {
      if (group.grouping_indicators[key_index]) continue;
      const auto source_column = request.group_key_terms[key_index].column;
      std::size_t value_bytes = 0;
      if (!AggregateTypedValuePayloadBytes(
              input_batch.rows[group.representative_row]
                  .values[source_column],
              &value_bytes) ||
          group_key_payload_bytes >
              std::numeric_limits<std::size_t>::max() - value_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "grouped aggregate key payload size overflowed"));
      }
      group_key_payload_bytes += value_bytes;
    }
    if (retained_output_payload_bytes >
            *node_memory_grant - fixed_retained_memory ||
        group_key_payload_bytes >
            *node_memory_grant - fixed_retained_memory -
                retained_output_payload_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "grouped aggregate retained output exhausted the selected-node grant"));
    }
    auto aggregate_result = ExecuteCanonicalAggregateRuntimeSelected(
        group_request, kernel_dag, group_input_batch, true, false,
        {scope_retained_memory + retained_output_payload_bytes +
             group_key_payload_bytes,
         remaining_finalization_bytes});
    if (!aggregate_result.diagnostic.ok) {
      return refuse(std::move(aggregate_result.diagnostic));
    }
    if (aggregate_result.output_batch.rows.size() != 1 ||
        aggregate_result.output_batch.rows.front().values.size() != 1 ||
        !aggregate_result.shared_state_authority_used) {
      return grouped_refusal(
          "shared aggregate state returned an invalid group result");
    }
    if (aggregate_result.final_output_bytes >
        maximum_combined_final_output_bytes -
            result.combined_final_output_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "combined grouped final output byte bound is exceeded"));
    }
    result.combined_final_output_bytes +=
        aggregate_result.final_output_bytes;
    result.peak_finalization_workspace_bytes = std::max(
        result.peak_finalization_workspace_bytes,
        aggregate_result.peak_finalization_workspace_bytes);
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
        value = input_batch.rows[group.representative_row]
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
    if (group_key_payload_bytes >
            *node_memory_grant - fixed_retained_memory -
                retained_output_payload_bytes ||
        aggregate_result.final_output_bytes >
            *node_memory_grant - fixed_retained_memory -
                retained_output_payload_bytes - group_key_payload_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "grouped aggregate output exhausted the selected-node memory grant"));
    }
    retained_output_payload_bytes +=
        group_key_payload_bytes + aggregate_result.final_output_bytes;
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
      aggregate.mga_authority, execution_dag);
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
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = aggregate_node->physical_node_id;
  result.causal_counter_id = aggregate_node->causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

CanonicalGroupedAggregateRuntimeResult ExecuteCanonicalGroupedAggregateRuntime(
    const CanonicalGroupedAggregateRuntimeRequest& request) {
  return ExecuteCanonicalGroupedAggregateRuntimeSelected(
      request, request.aggregate_request.physical_dag,
      request.aggregate_request.input_batch, false, false, 0);
}

CanonicalGroupedAggregateRuntimeResult ExecuteCanonicalGroupedAggregateRuntime(
    const CanonicalGroupedAggregateRuntimeRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalGroupedAggregateRuntimeSelected(
      request, borrowed_execution_dag, borrowed_input_batch, true, false, 0);
}

// QOW-SOURCE-QRY-011-GROUPED-SET-V1
// Compose several registry aggregates over one exact grouped physical node.
// Group identity must match across every independently filtered/ordered state;
// aggregate values are then appended in the physical output-descriptor order.
static CanonicalGroupedAggregateSetRuntimeResult
ExecuteCanonicalGroupedAggregateSetRuntimeSelected(
    const CanonicalGroupedAggregateSetRuntimeRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers,
    const bool spill_execution_context,
    const std::size_t retained_memory_bytes = 0) {
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
    result.combined_final_output_bytes = 0;
    result.peak_finalization_workspace_bytes = 0;
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
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(common.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(common.input_batch))) {
    return set_refusal(
        "grouped aggregate-set request carries conflicting owned execution carriers");
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      common.mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const PhysicalNodeRecord* aggregate_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == common.selected_physical_node_id) {
      aggregate_node = &node;
      break;
    }
  }
  if (common.selected_physical_node_id == 0 ||
      common.selected_physical_node_id !=
          execution_dag.root_physical_node_id ||
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

  const auto node_memory_grant =
      AggregateNodeMemoryGrant(execution_dag, *aggregate_node);
  std::size_t input_payload_bytes = 0;
  if (!node_memory_grant.has_value() ||
      !AggregateBatchPayloadBytes(execution_input_batch,
                                  &input_payload_bytes) ||
      retained_memory_bytes > *node_memory_grant ||
      common.retained_memory_bytes >
          *node_memory_grant - retained_memory_bytes ||
      input_payload_bytes >
          *node_memory_grant - retained_memory_bytes -
              common.retained_memory_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate-set selected-node memory grant is invalid or exhausted"));
  }
  const auto maximum_combined_final_output_bytes =
      request.maximum_combined_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(request.maximum_combined_final_output_bytes,
                     *node_memory_grant);

  std::vector<const CanonicalAggregateRuntimeRequest*> aggregate_specs = {
      &common};
  aggregate_specs.reserve(aggregate_count);
  const auto has_shadow_authority = [](const auto& specification) {
    return specification.selected_physical_node_id != 0 ||
           specification.retained_memory_bytes != 0 ||
           !TypedPhysicalNodeDagCarrierIsExactDefault(
               specification.physical_dag) ||
           !DescriptorBatchCarrierIsExactDefault(specification.input_batch);
  };
  for (const auto& specification : request.additional_aggregates) {
    if (has_shadow_authority(specification)) {
      return set_refusal(
          "additional aggregate carries shadow physical or input authority");
    }
    const auto additional_authority = RevalidateCanonicalExecutionMgaAuthority(
        specification.mga_authority, execution_dag);
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

  std::size_t total_source_filter_bytes = 0;
  for (const auto* specification : aggregate_specs) {
    if (!specification->filter_truth_values.has_value()) continue;
    if (specification->filter_truth_values->size() !=
        execution_input_batch.rows.size()) {
      return set_refusal(
          "aggregate-set FILTER cardinality is not bound to the common input");
    }
    std::size_t filter_bytes = 0;
    if (!CheckedAggregateFinalizationMultiply(
            specification->filter_truth_values->size(),
            sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
            &filter_bytes) ||
        !CheckedAggregateFinalizationAdd(&total_source_filter_bytes,
                                         filter_bytes)) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "aggregate-set FILTER memory size overflowed"));
    }
  }
  if (total_source_filter_bytes >
      *node_memory_grant - retained_memory_bytes -
          common.retained_memory_bytes - input_payload_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "aggregate-set FILTER memory exceeds the selected-node grant"));
  }

  const auto within_total = [](const std::size_t next,
                               const std::size_t current,
                               const std::size_t maximum) {
    return current <= maximum && next <= maximum - current;
  };
  std::vector<CanonicalGroupedAggregateRuntimeResult> executions;
  executions.reserve(aggregate_count);
  std::size_t retained_execution_payload_bytes = 0;
  for (std::size_t index = 0; index < aggregate_specs.size(); ++index) {
    CanonicalGroupedAggregateRuntimeRequest grouped_request;
    grouped_request.group_key_terms = first.group_key_terms;
    grouped_request.group_result_columns = first.group_result_columns;
    grouped_request.grouping_sets = first.grouping_sets;
    grouped_request.maximum_grouping_set_count =
        first.maximum_grouping_set_count;
    grouped_request.maximum_grouping_set_member_count =
        first.maximum_grouping_set_member_count;
    grouped_request.maximum_group_count = first.maximum_group_count;
    grouped_request.maximum_grouping_key_comparison_count =
        first.maximum_grouping_key_comparison_count;
    grouped_request.maximum_grouping_set_transition_count =
        first.maximum_grouping_set_transition_count;
    grouped_request.maximum_combined_distinct_tuple_count =
        first.maximum_combined_distinct_tuple_count;
    grouped_request.maximum_combined_order_comparison_count =
        first.maximum_combined_order_comparison_count;
    grouped_request.maximum_combined_state_bytes =
        first.maximum_combined_state_bytes;
    grouped_request.maximum_combined_final_output_bytes =
        first.maximum_combined_final_output_bytes;
    grouped_request.maximum_output_rows = first.maximum_output_rows;
    grouped_request.aggregate_request =
        CloneCanonicalAggregateRequestWithoutRows(*aggregate_specs[index],
                                                   false);
    if (aggregate_specs[index]->filter_truth_values.has_value()) {
      grouped_request.aggregate_request.filter_truth_values =
          std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>{};
    }
    grouped_request.aggregate_request.selected_physical_node_id =
        common.selected_physical_node_id;
    grouped_request.aggregate_request.retained_memory_bytes =
        common.retained_memory_bytes;
    grouped_request.aggregate_request.mga_authority = common.mga_authority;
    const auto remaining_final_output_bytes =
        maximum_combined_final_output_bytes -
        result.combined_final_output_bytes;
    std::size_t current_source_filter_bytes = 0;
    if (aggregate_specs[index]->filter_truth_values.has_value() &&
        !CheckedAggregateFinalizationMultiply(
            aggregate_specs[index]->filter_truth_values->size(),
            sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue),
            &current_source_filter_bytes)) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "aggregate-set current FILTER memory size overflowed"));
    }
    auto grouped_execution_dag = execution_dag;
    for (auto& node : grouped_execution_dag.nodes) {
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
        grouped_request, grouped_execution_dag, execution_input_batch, true,
        spill_execution_context,
        retained_memory_bytes + retained_execution_payload_bytes +
            total_source_filter_bytes - current_source_filter_bytes,
        remaining_final_output_bytes,
        aggregate_specs[index]->filter_truth_values.has_value()
            ? &*aggregate_specs[index]->filter_truth_values
            : nullptr);
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
                      request.maximum_combined_state_bytes) ||
        !within_total(execution.combined_final_output_bytes,
                      result.combined_final_output_bytes,
                      maximum_combined_final_output_bytes)) {
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
    result.combined_final_output_bytes +=
        execution.combined_final_output_bytes;
    result.peak_finalization_workspace_bytes = std::max(
        result.peak_finalization_workspace_bytes,
        execution.peak_finalization_workspace_bytes);
    std::size_t execution_payload_bytes = 0;
    if (!AggregateBatchPayloadBytes(execution.output_batch,
                                    &execution_payload_bytes) ||
        retained_execution_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                execution_payload_bytes ||
        retained_memory_bytes + common.retained_memory_bytes +
                input_payload_bytes + total_source_filter_bytes >
            *node_memory_grant ||
        retained_execution_payload_bytes + execution_payload_bytes >
            *node_memory_grant - retained_memory_bytes -
                common.retained_memory_bytes -
                input_payload_bytes - total_source_filter_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "aggregate-set retained output exhausted the selected-node grant"));
    }
    retained_execution_payload_bytes += execution_payload_bytes;
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
    auto& identity_values =
        executions.front().output_batch.rows[group].values;
    row.values.insert(
        row.values.end(),
        std::make_move_iterator(identity_values.begin()),
        std::make_move_iterator(
            identity_values.begin() +
            static_cast<std::ptrdiff_t>(key_count)));
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
    for (auto& execution : executions) {
      row.values.push_back(std::move(
          execution.output_batch.rows[group].values[key_count]));
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
      common.mga_authority, execution_dag);
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
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = aggregate_node->physical_node_id;
  result.causal_counter_id = aggregate_node->causal_counter_id;
  result.mga_statement_context = common.mga_authority.statement_context;
  return result;
}

CanonicalGroupedAggregateSetRuntimeResult
ExecuteCanonicalGroupedAggregateSetRuntime(
    const CanonicalGroupedAggregateSetRuntimeRequest& request) {
  return ExecuteCanonicalGroupedAggregateSetRuntimeSelected(
      request, request.first_aggregate.aggregate_request.physical_dag,
      request.first_aggregate.aggregate_request.input_batch, false, false, 0);
}

CanonicalGroupedAggregateSetRuntimeResult
ExecuteCanonicalGroupedAggregateSetRuntime(
    const CanonicalGroupedAggregateSetRuntimeRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalGroupedAggregateSetRuntimeSelected(
      request, borrowed_execution_dag, borrowed_input_batch, true, false, 0);
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

  auto baseline = ExecuteCanonicalGroupedAggregateSetRuntimeSelected(
      request.grouped_request, entry_common.physical_dag,
      entry_common.input_batch, false, true, 0);
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

  std::vector<const CanonicalAggregateRuntimeRequest*> aggregate_specs;
  aggregate_specs.reserve(baseline.aggregate_count);
  aggregate_specs.push_back(&common);
  for (const auto& additional : request.grouped_request.additional_aggregates) {
    aggregate_specs.push_back(&additional);
  }
  if (aggregate_specs.size() != baseline.aggregate_count) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EVIDENCE-V1",
                  "grouped aggregate state inventory diverges from the canonical result");
  }

  const auto expected_aggregate_count = baseline.aggregate_count;
  const auto expected_aggregate_transition_count =
      baseline.aggregate_transition_count;
  const auto expected_distinct_tuple_count =
      baseline.aggregate_distinct_tuple_count;
  const auto expected_order_comparison_count =
      baseline.aggregate_order_comparison_count;
  const auto expected_combined_state_bytes = baseline.combined_state_bytes;
  const auto expected_combined_final_output_bytes =
      baseline.combined_final_output_bytes;
  const auto baseline_peak_finalization_workspace_bytes =
      baseline.peak_finalization_workspace_bytes;
  std::size_t baseline_output_payload_bytes = 0;
  std::size_t input_payload_bytes = 0;
  std::size_t total_source_filter_bytes = 0;
  if (!AggregateBatchPayloadBytes(baseline.output_batch,
                                  &baseline_output_payload_bytes) ||
      !AggregateBatchPayloadBytes(common.input_batch,
                                  &input_payload_bytes)) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                  "grouped aggregate retained memory size overflowed");
  }
  for (const auto* specification : aggregate_specs) {
    if (!specification->filter_truth_values.has_value()) continue;
    if (specification->filter_truth_values->size() !=
        common.input_batch.rows.size()) {
      return refuse("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                    "grouped aggregate spill FILTER cardinality is not bound");
    }
    std::size_t filter_bytes = 0;
    if (!CheckedAggregateFinalizationMultiply(
            specification->filter_truth_values->size(),
            sizeof(EngineSqlTruthValue), &filter_bytes) ||
        !CheckedAggregateFinalizationAdd(&total_source_filter_bytes,
                                         filter_bytes)) {
      return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                    "grouped aggregate FILTER memory size overflowed");
    }
  }
  if (baseline_output_payload_bytes >
          std::numeric_limits<std::size_t>::max() - input_payload_bytes ||
      total_source_filter_bytes >
          std::numeric_limits<std::size_t>::max() -
              baseline_output_payload_bytes - input_payload_bytes) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                  "grouped aggregate retained memory size overflowed");
  }
  const auto replay_retained_memory_bytes =
      baseline_output_payload_bytes + input_payload_bytes +
      total_source_filter_bytes;
  const PhysicalNodeRecord* aggregate_node = nullptr;
  for (const auto& node : common.physical_dag.nodes) {
    if (node.physical_node_id == common.selected_physical_node_id) {
      aggregate_node = &node;
      break;
    }
  }
  const auto node_memory_grant =
      aggregate_node == nullptr
          ? std::optional<std::size_t>{}
          : AggregateNodeMemoryGrant(common.physical_dag, *aggregate_node);
  if (!node_memory_grant.has_value() ||
      replay_retained_memory_bytes > *node_memory_grant ||
      common.retained_memory_bytes >
          *node_memory_grant - replay_retained_memory_bytes) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                  "grouped aggregate replay retention exhausts the selected-node grant");
  }
  auto restored = std::move(baseline);
  std::size_t aggregate_transition_count = 0;
  std::size_t distinct_tuple_count = 0;
  std::size_t order_comparison_count = 0;
  std::size_t combined_state_bytes = 0;
  std::size_t replayed_combined_final_output_bytes = 0;
  std::size_t replay_peak_finalization_workspace_bytes = 0;
  const auto within_total = [](const std::size_t next,
                               const std::size_t current,
                               const std::size_t maximum) {
    return current <= maximum && next <= maximum - current;
  };
  const auto key_count = first.group_result_columns.size();
  for (std::size_t group_index = 0;
       group_index < restored.groups.size(); ++group_index) {
    const auto& group = restored.groups[group_index];
    if (group.source_row_count != group.source_row_indices.size() ||
        restored.output_batch.rows[group_index].values.size() !=
            key_count + expected_aggregate_count ||
        group.aggregate_transition_counts.size() != expected_aggregate_count ||
        group.aggregate_state_bytes.size() != expected_aggregate_count) {
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
      const auto& source_aggregate = *aggregate_specs[aggregate_index];
      const auto* full_filter =
          source_aggregate.filter_truth_values.has_value()
              ? &*source_aggregate.filter_truth_values
              : nullptr;
      std::size_t group_input_payload_bytes = 1;
      for (const auto row : group.source_row_indices) {
        if (row >= common.input_batch.rows.size()) {
          return refuse(
              "QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EVIDENCE-V1",
              "grouped aggregate source-row identity is out of range");
        }
        for (const auto& value : common.input_batch.rows[row].values) {
          if (!CheckedAggregateFinalizationAdd(
                  &group_input_payload_bytes,
                  value.encoded_value.size()) ||
              !CheckedAggregateFinalizationAdd(
                  &group_input_payload_bytes,
                  value.binary_value.size())) {
            return refuse(
                "QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                "grouped aggregate replay input size overflowed");
          }
        }
      }
      std::size_t group_filter_bytes = 0;
      if (full_filter != nullptr &&
          !CheckedAggregateFinalizationMultiply(
              group.source_row_indices.size(), sizeof(EngineSqlTruthValue),
              &group_filter_bytes)) {
        return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                      "grouped aggregate replay FILTER size overflowed");
      }
      const auto replay_fixed_memory = replay_retained_memory_bytes +
                                       common.retained_memory_bytes;
      if (group_input_payload_bytes >
              *node_memory_grant - replay_fixed_memory ||
          group_filter_bytes >
              *node_memory_grant - replay_fixed_memory -
                  group_input_payload_bytes) {
        return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-RESOURCE-V1",
                      "grouped aggregate replay copy exceeds the selected-node grant");
      }
      auto aggregate =
          CloneCanonicalAggregateRequestWithoutRows(source_aggregate, false);
      aggregate.selected_physical_node_id =
          common.selected_physical_node_id;
      aggregate.retained_memory_bytes = common.retained_memory_bytes;
      DescriptorBatch group_input_batch;
      group_input_batch.columns = common.input_batch.columns;
      group_input_batch.rows.reserve(group.source_row_indices.size());
      for (const auto row : group.source_row_indices) {
        group_input_batch.rows.push_back(common.input_batch.rows[row]);
      }
      if (full_filter != nullptr) {
        std::vector<EngineSqlTruthValue> group_filter;
        group_filter.reserve(group.source_row_indices.size());
        for (const auto row : group.source_row_indices) {
          group_filter.push_back((*full_filter)[row]);
        }
        aggregate.filter_truth_values = std::move(group_filter);
      }
      auto replay_execution_dag = common.physical_dag;
      for (auto& node : replay_execution_dag.nodes) {
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
      state_request.retained_memory_bytes = replay_retained_memory_bytes;
      state_request.cancellation_requested = request.cancellation_requested;
      state_request.cleanup_after_cancellation =
          request.cleanup_after_cancellation;
      state_request.restart_recovery_proof_available =
          request.restart_recovery_proof_available;
      auto state = ExecuteCanonicalAggregateStateSpill(
          state_request, replay_execution_dag, group_input_batch);

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
              restored.selected_plan_uuid ||
          state.aggregate_result.executed_physical_node_id !=
              restored.executed_physical_node_id ||
          state.aggregate_result.causal_counter_id !=
              restored.causal_counter_id ||
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
          restored.output_batch.rows[group_index]
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
                        expected_aggregate_transition_count) ||
          !within_total(state.aggregate_result.distinct_tuple_count,
                        distinct_tuple_count,
                        expected_distinct_tuple_count) ||
          !within_total(state.aggregate_result.order_comparison_count,
                        order_comparison_count,
                        expected_order_comparison_count) ||
          !within_total(state.aggregate_result.state_bytes,
                        combined_state_bytes,
                        expected_combined_state_bytes) ||
          !within_total(state.aggregate_result.final_output_bytes,
                        replayed_combined_final_output_bytes,
                        expected_combined_final_output_bytes)) {
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
      replayed_combined_final_output_bytes +=
          state.aggregate_result.final_output_bytes;
      replay_peak_finalization_workspace_bytes = std::max(
          replay_peak_finalization_workspace_bytes,
          state.aggregate_result.peak_finalization_workspace_bytes);
      restored.output_batch.rows[group_index]
          .values[key_count + aggregate_index] = std::move(
              state.aggregate_result.output_batch.rows.front().values.front());
    }
  }

  if (aggregate_transition_count != expected_aggregate_transition_count ||
      distinct_tuple_count != expected_distinct_tuple_count ||
      order_comparison_count != expected_order_comparison_count ||
      combined_state_bytes != expected_combined_state_bytes ||
      replayed_combined_final_output_bytes !=
          expected_combined_final_output_bytes ||
      restored.combined_final_output_bytes !=
          expected_combined_final_output_bytes ||
      replay_peak_finalization_workspace_bytes !=
          baseline_peak_finalization_workspace_bytes) {
    return refuse("QOW-DIAG-QRY-011-GROUPED-STATE-SPILL-EQUIVALENCE-V1",
                  "restored grouped aggregate totals diverge from the in-memory result");
  }
  restored.peak_finalization_workspace_bytes =
      replay_peak_finalization_workspace_bytes;
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
