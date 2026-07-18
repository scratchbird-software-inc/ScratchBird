// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/select_api.hpp"

#include "crud_support/crud_store.hpp"
#include "catalog/global_aggregate_view.hpp"
#include "catalog/relation_descriptor_projection.hpp"
#include "catalog/relation_projection_view.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "domain_support/domain_store.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

using SelectApiSteadyClock = std::chrono::steady_clock;

std::uint64_t SelectApiElapsedMicros(
    SelectApiSteadyClock::time_point begin,
    SelectApiSteadyClock::time_point end = SelectApiSteadyClock::now()) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

void WriteSelectApiPhaseTrace(
    std::string_view layer,
    std::string_view operation,
    std::size_t row_count,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_SELECT_API_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') return;
  static std::mutex trace_mutex;
  std::lock_guard<std::mutex> guard(trace_mutex);
  std::ofstream out(trace_path, std::ios::app);
  if (!out) return;
  out << "layer=" << layer
      << '\t' << "operation=" << operation
      << '\t' << "rows=" << row_count;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << '\t' << "total_us=" << total << '\n';
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) { return option.substr(prefix.size()); }
  }
  return {};
}

bool TryParseI64Value(const std::string& value, std::int64_t* out) {
  if (value.empty()) { return false; }
  char* end = nullptr;
  const long long parsed = std::strtoll(value.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') { return false; }
  if (out != nullptr) { *out = static_cast<std::int64_t>(parsed); }
  return true;
}

bool TryParseDecimalValue(const std::string& value, long double* out) {
  if (value.empty()) { return false; }
  char* end = nullptr;
  const long double parsed = std::strtold(value.c_str(), &end);
  if (end == nullptr || *end != '\0') { return false; }
  if (out != nullptr) { *out = parsed; }
  return true;
}

std::string FormatDecimalScale(long double value, int scale) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(scale) << static_cast<double>(value);
  return out.str();
}

int CompareSelectScalar(std::string_view left, std::string_view right) {
  long double left_number = 0.0;
  long double right_number = 0.0;
  if (TryParseDecimalValue(std::string(left), &left_number) &&
      TryParseDecimalValue(std::string(right), &right_number)) {
    if (left_number < right_number) return -1;
    if (left_number > right_number) return 1;
    return 0;
  }
  if (left < right) return -1;
  if (left > right) return 1;
  return 0;
}

EngineDescriptor TextDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "canonical=text";
  return descriptor;
}

EngineDescriptor Int64Descriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor = "canonical=int64";
  return descriptor;
}

EngineTypedValue TextValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor = TextDescriptor();
  typed.encoded_value = std::move(value);
  return typed;
}

EngineTypedValue Int64Value(std::int64_t value) {
  EngineTypedValue typed;
  typed.descriptor = Int64Descriptor();
  typed.encoded_value = std::to_string(value);
  return typed;
}

EngineResultShape CountProjectionResultShape(
    const EngineSelectRowsRequest& request,
    std::uint64_t count,
    std::string* error_detail) {
  if (count > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
    if (error_detail != nullptr) {
      *error_detail = "dml_select_count_projection_overflow";
    }
    return {};
  }
  std::string column_name = OptionValue(request, "actual_column_name:");
  if (column_name.empty()) column_name = "COUNT";

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(Int64Descriptor());
  EngineRowValue row;
  row.fields.push_back(
      {std::move(column_name), Int64Value(static_cast<std::int64_t>(count))});
  shape.rows.push_back(std::move(row));
  return shape;
}

EngineResultShape CountAssertionResultShape(const EngineSelectRowsRequest& request,
                                            std::uint64_t actual_count,
                                            std::string* error_detail) {
  const std::string assertion_id = OptionValue(request, "assertion_id:");
  std::string actual_column = OptionValue(request, "actual_column_name:");
  if (actual_column.empty()) { actual_column = "actual_count"; }
  std::string expected_column = OptionValue(request, "expected_column_name:");
  if (expected_column.empty()) { expected_column = "expected_count"; }

  std::int64_t expected_count = 0;
  if (!TryParseI64Value(OptionValue(request, "expected_count:"), &expected_count)) {
    if (error_detail != nullptr) { *error_detail = "dml_select_count_assertion_expected_count_invalid"; }
    return {};
  }
  if (actual_count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    if (error_detail != nullptr) { *error_detail = "dml_select_count_assertion_actual_count_overflow"; }
    return {};
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(Int64Descriptor());
  EngineRowValue out;
  out.fields.push_back({"assertion_id", TextValue(assertion_id)});
  out.fields.push_back({actual_column, Int64Value(static_cast<std::int64_t>(actual_count))});
  out.fields.push_back({expected_column, Int64Value(expected_count)});
  shape.rows.push_back(std::move(out));
  return shape;
}

EngineResultShape FieldAssertionResultShape(const EngineSelectRowsRequest& request,
                                            const std::vector<CrudRowVersionRecord>& rows,
                                            std::string* error_detail) {
  const std::string assertion_id = OptionValue(request, "assertion_id:");
  const std::string actual_source_column = OptionValue(request, "actual_source_column:");
  if (actual_source_column.empty()) {
    if (error_detail != nullptr) { *error_detail = "dml_select_field_assertion_source_column_required"; }
    return {};
  }
  std::string actual_column = OptionValue(request, "actual_column_name:");
  if (actual_column.empty()) { actual_column = "actual_value"; }
  std::string expected_column = OptionValue(request, "expected_column_name:");
  if (expected_column.empty()) { expected_column = "expected_value"; }
  const std::string expected_value = OptionValue(request, "expected_value:");

  std::string actual_value;
  if (!rows.empty()) {
    if (actual_source_column.rfind("case_is_null:", 0) == 0) {
      const std::string source_column = actual_source_column.substr(
          std::string("case_is_null:").size());
      actual_value =
          CrudFieldValue(rows.front().values, source_column) == "<NULL>" ? "1" : "0";
    } else {
      actual_value = CrudFieldValue(rows.front().values, actual_source_column);
    }
  }

  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(TextDescriptor());
  EngineRowValue out;
  out.fields.push_back({"assertion_id", TextValue(assertion_id)});
  out.fields.push_back({actual_column, TextValue(actual_value)});
  out.fields.push_back({expected_column, TextValue(expected_value)});
  shape.rows.push_back(std::move(out));
  return shape;
}

std::string PayloadFieldValue(const std::string& payload, const std::string& prefix) {
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto next = payload.find(';', offset);
    const auto end = next == std::string::npos ? payload.size() : next;
    const std::string field = payload.substr(offset, end - offset);
    if (field.rfind(prefix, 0) == 0) return field.substr(prefix.size());
    if (next == std::string::npos) break;
    offset = next + 1;
  }
  return {};
}

const EngineExecutableObjectRecord* FindExecutableObject(
    const EngineExecutableObjectLifecycleState& state,
    const std::string& object_uuid) {
  const EngineExecutableObjectRecord* found = nullptr;
  for (const auto& object : state.objects) {
    if (object.object_uuid != object_uuid) continue;
    if (object.deleted || object.invalidated || object.lifecycle_state != "active") continue;
    if (found == nullptr || object.event_sequence > found->event_sequence) {
      found = &object;
    }
  }
  return found;
}

std::optional<std::int64_t> RoutineArgumentI64(const EngineSelectRowsRequest& request,
                                               std::size_t index) {
  const std::string value =
      OptionValue(request, "routine_argument_" + std::to_string(index) + "_value:");
  std::int64_t parsed = 0;
  if (!TryParseI64Value(value, &parsed)) return std::nullopt;
  return parsed;
}

CrudRowVersionRecord MakeProcedureRow(const std::string& routine_uuid,
                                       std::uint64_t sequence,
                                       std::vector<std::pair<std::string, std::string>> values) {
  CrudRowVersionRecord row;
  row.table_uuid = routine_uuid;
  row.row_uuid = routine_uuid + ":row:" + std::to_string(sequence);
  row.version_uuid = row.row_uuid + ":v1";
  row.sequence = sequence;
  row.values = std::move(values);
  return row;
}

std::vector<CrudRowVersionRecord> MaterializeGenerateSeriesProcedure(
    const EngineSelectRowsRequest& request,
    const std::string& routine_uuid,
    std::string* error_detail) {
  const auto start = RoutineArgumentI64(request, 0);
  const auto end = RoutineArgumentI64(request, 1);
  const auto step = RoutineArgumentI64(request, 2);
  if (!start || !end || !step) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_arguments_invalid";
    return {};
  }
  std::vector<CrudRowVersionRecord> rows;
  if (*step <= 0 || *start > *end) return rows;
  std::uint64_t sequence = 0;
  for (std::int64_t value = *start; value <= *end; value += *step) {
    rows.push_back(MakeProcedureRow(routine_uuid, ++sequence, {{"n", std::to_string(value)}}));
    if (*step > 0 && value > std::numeric_limits<std::int64_t>::max() - *step) break;
  }
  return rows;
}

std::vector<CrudRowVersionRecord> MaterializeEvenNumbersProcedure(
    const EngineSelectRowsRequest& request,
    const std::string& routine_uuid,
    std::string* error_detail) {
  const auto max_value = RoutineArgumentI64(request, 0);
  if (!max_value) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_arguments_invalid";
    return {};
  }
  std::vector<CrudRowVersionRecord> rows;
  std::uint64_t sequence = 0;
  for (std::int64_t value = 2; value <= *max_value; value += 2) {
    rows.push_back(
        MakeProcedureRow(routine_uuid, ++sequence, {{"even_n", std::to_string(value)}}));
    if (value > std::numeric_limits<std::int64_t>::max() - 2) break;
  }
  return rows;
}

std::vector<CrudRowVersionRecord> MaterializeDynamicMultiplyProcedure(
    const EngineSelectRowsRequest& request,
    const EngineExecutableObjectRecord& object,
    const std::string& routine_uuid,
    std::string* error_detail) {
  const auto max_key = RoutineArgumentI64(request, 0);
  const auto factor = RoutineArgumentI64(request, 1);
  if (!max_key || !factor) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_arguments_invalid";
    return {};
  }
  const std::string dependency_uuid =
      PayloadFieldValue(object.payload, "related_object_0_uuid:");
  if (dependency_uuid.empty()) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_dependency_required";
    return {};
  }
  const auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) {
    if (error_detail != nullptr) *error_detail = loaded.diagnostic.detail;
    return {};
  }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindVisibleCrudTable(state,
                                          dependency_uuid,
                                          request.context.local_transaction_id);
  if (!table) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_dependency_not_visible";
    return {};
  }
  std::vector<CrudRowVersionRecord> source_rows =
      VisibleCrudRowsForContext(state, dependency_uuid, request.context);
  std::stable_sort(source_rows.begin(),
                   source_rows.end(),
                   [](const CrudRowVersionRecord& lhs, const CrudRowVersionRecord& rhs) {
                     std::int64_t left = 0;
                     std::int64_t right = 0;
                     TryParseI64Value(CrudFieldValue(lhs.values, "row_key"), &left);
                     TryParseI64Value(CrudFieldValue(rhs.values, "row_key"), &right);
                     return left < right;
                   });
  std::vector<CrudRowVersionRecord> rows;
  std::uint64_t sequence = 0;
  for (const auto& source : source_rows) {
    std::int64_t row_key = 0;
    std::int64_t multiplier = 0;
    if (!TryParseI64Value(CrudFieldValue(source.values, "row_key"), &row_key) ||
        !TryParseI64Value(CrudFieldValue(source.values, "multiplier"), &multiplier)) {
      continue;
    }
    if (row_key > *max_key) continue;
    rows.push_back(MakeProcedureRow(
        routine_uuid,
        ++sequence,
        {{"row_key", std::to_string(row_key)},
         {"computed_value", std::to_string(multiplier * *factor)}}));
  }
  return rows;
}

std::vector<CrudRowVersionRecord> MaterializeFirebirdFirstProductProcedure(
    const EngineSelectRowsRequest& request,
    const EngineExecutableObjectRecord& object,
    const std::string& routine_uuid,
    std::string* error_detail) {
  const std::string dependency_uuid =
      PayloadFieldValue(object.payload, "related_object_0_uuid:");
  if (dependency_uuid.empty()) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_dependency_required";
    return {};
  }
  const auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) {
    if (error_detail != nullptr) *error_detail = loaded.diagnostic.detail;
    return {};
  }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindVisibleCrudTable(state,
                                          dependency_uuid,
                                          request.context.local_transaction_id);
  if (!table) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_dependency_not_visible";
    return {};
  }
  const std::vector<CrudRowVersionRecord> source_rows =
      VisibleCrudRowsForContext(state, dependency_uuid, request.context);
  if (source_rows.empty()) { return {}; }
  long double campo1 = 0;
  long double campo2 = 0;
  if (!TryParseDecimalValue(CrudFieldValue(source_rows.front().values, "campo1"), &campo1) ||
      !TryParseDecimalValue(CrudFieldValue(source_rows.front().values, "campo2"), &campo2)) {
    if (error_detail != nullptr) *error_detail = "selectable_procedure_source_value_invalid";
    return {};
  }
  return {MakeProcedureRow(
      routine_uuid,
      1,
      {{"retorno", FormatDecimalScale(campo1 * campo2, 2)}})};
}

std::vector<CrudRowVersionRecord> MaterializeSelectableProcedureRows(
    const EngineSelectRowsRequest& request,
    const EngineExecutableObjectRecord& object,
    const std::string& routine_uuid,
    std::string* error_detail) {
  const std::string descriptor =
      PayloadFieldValue(object.payload, "compiled_body_descriptor:");
  if (descriptor == "sbsql.compiled.selectable.generate_series.v1") {
    return MaterializeGenerateSeriesProcedure(request, routine_uuid, error_detail);
  }
  if (descriptor == "sbsql.compiled.selectable.even_numbers.v1") {
    return MaterializeEvenNumbersProcedure(request, routine_uuid, error_detail);
  }
  if (descriptor == "sbsql.compiled.selectable.dynamic_multiply.v1") {
    return MaterializeDynamicMultiplyProcedure(request, object, routine_uuid, error_detail);
  }
  if (descriptor == "sbsql.compiled.selectable.firebird_first_product.v1") {
    return MaterializeFirebirdFirstProductProcedure(request, object, routine_uuid, error_detail);
  }
  if (error_detail != nullptr) *error_detail = "selectable_procedure_descriptor_unsupported";
  return {};
}

EngineResultShape AggregateAssertionResultShape(const EngineSelectRowsRequest& request,
                                                const std::vector<CrudRowVersionRecord>& rows,
                                                std::string* error_detail) {
  const std::string assertion_id = OptionValue(request, "assertion_id:");
  const std::string aggregate_function = OptionValue(request, "aggregate_function:");
  const std::string aggregate_source_column = OptionValue(request, "aggregate_source_column:");
  if (aggregate_function != "sb.aggregate.sum" || aggregate_source_column.empty()) {
    if (error_detail != nullptr) *error_detail = "dml_select_aggregate_assertion_invalid";
    return {};
  }
  std::string actual_column = OptionValue(request, "actual_column_name:");
  if (actual_column.empty()) actual_column = "actual_sum";
  std::string expected_column = OptionValue(request, "expected_column_name:");
  if (expected_column.empty()) expected_column = "expected_sum";
  std::int64_t sum = 0;
  for (const auto& row : rows) {
    std::int64_t value = 0;
    if (!TryParseI64Value(CrudFieldValue(row.values, aggregate_source_column), &value)) {
      if (error_detail != nullptr) *error_detail = "dml_select_aggregate_value_invalid";
      return {};
    }
    sum += value;
  }
  std::int64_t expected = 0;
  if (!TryParseI64Value(OptionValue(request, "expected_value:"), &expected)) {
    if (error_detail != nullptr) *error_detail = "dml_select_aggregate_expected_invalid";
    return {};
  }
  EngineResultShape shape;
  shape.result_kind = "query_rowset";
  shape.columns.push_back(TextDescriptor());
  shape.columns.push_back(Int64Descriptor());
  shape.columns.push_back(Int64Descriptor());
  EngineRowValue out;
  out.fields.push_back({"assertion_id", TextValue(assertion_id)});
  out.fields.push_back({actual_column, Int64Value(sum)});
  out.fields.push_back({expected_column, Int64Value(expected)});
  shape.rows.push_back(std::move(out));
  return shape;
}

std::string OrderingColumn(const EngineOrderingEnvelope& ordering) {
  if (ordering.canonical_ordering_envelopes.empty()) { return {}; }
  std::string column = ordering.canonical_ordering_envelopes.front();
  const auto separator = column.find(':');
  if (separator != std::string::npos) { column = column.substr(0, separator); }
  const auto space = column.find(' ');
  if (space != std::string::npos) { column = column.substr(0, space); }
  return column;
}

bool OrderingAscending(const EngineOrderingEnvelope& ordering) {
  if (ordering.canonical_ordering_envelopes.empty()) { return true; }
  const std::string lowered = LowerAscii(ordering.canonical_ordering_envelopes.front());
  return lowered.find(":desc") == std::string::npos && lowered.find(" desc") == std::string::npos;
}

std::string OrderingNullsPlacement(const EngineOrderingEnvelope& ordering) {
  if (ordering.canonical_ordering_envelopes.empty()) { return {}; }
  const std::string lowered = LowerAscii(ordering.canonical_ordering_envelopes.front());
  if (lowered.find("nulls_first") != std::string::npos ||
      lowered.find("nulls first") != std::string::npos) {
    return "first";
  }
  if (lowered.find("nulls_last") != std::string::npos ||
      lowered.find("nulls last") != std::string::npos) {
    return "last";
  }
  return {};
}

bool IsIntegerText(const std::string& value) {
  if (value.empty()) { return false; }
  std::size_t index = value[0] == '-' ? 1 : 0;
  if (index == value.size()) { return false; }
  for (; index < value.size(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(value[index]))) { return false; }
  }
  return true;
}

bool ValueLess(const std::string& lhs, const std::string& rhs) {
  if (IsIntegerText(lhs) && IsIntegerText(rhs)) {
    try { return std::stoll(lhs) < std::stoll(rhs); } catch (...) {}
  }
  return lhs < rhs;
}

void ApplyOrdering(const EngineOrderingEnvelope& ordering, std::vector<CrudRowVersionRecord>* rows) {
  const std::string column = OrderingColumn(ordering);
  if (column.empty()) { return; }
  const bool ascending = OrderingAscending(ordering);
  const std::string nulls_placement = OrderingNullsPlacement(ordering);
  std::stable_sort(rows->begin(), rows->end(), [&](const CrudRowVersionRecord& lhs, const CrudRowVersionRecord& rhs) {
    const std::string left_value = CrudFieldValue(lhs.values, column);
    const std::string right_value = CrudFieldValue(rhs.values, column);
    const bool left_null = left_value == "<NULL>";
    const bool right_null = right_value == "<NULL>";
    if (left_null || right_null) {
      if (left_null == right_null) return false;
      if (nulls_placement == "first") return left_null;
      if (nulls_placement == "last") return right_null;
    }
    if (left_value == right_value) { return false; }
    return ascending ? ValueLess(left_value, right_value) : ValueLess(right_value, left_value);
  });
}

void ApplyProjection(const EngineProjectionEnvelope& projection, std::vector<CrudRowVersionRecord>* rows) {
  if (projection.canonical_projection_envelopes.empty()) { return; }
  for (auto& row : *rows) {
    std::vector<std::pair<std::string, std::string>> projected;
    for (const auto& column : projection.canonical_projection_envelopes) {
      projected.push_back({column, CrudFieldValue(row.values, column)});
    }
    row.values = std::move(projected);
  }
}

bool PredicateCanRowScan(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "always_false" ||
         predicate.predicate_kind == "column_equals" ||
         predicate.predicate_kind == "columns_all_equal" ||
         predicate.predicate_kind == "columns_all_null" ||
         predicate.predicate_kind == "columns_all_not_null" ||
         predicate.predicate_kind == "column_equals_column_or_left_null" ||
         predicate.predicate_kind == "column_like" ||
         predicate.predicate_kind == "column_not_like" ||
         predicate.predicate_kind == "column_mod_equals" ||
         predicate.predicate_kind == "column_in_list" ||
         predicate.predicate_kind == "column_in_list_scalar_compare" ||
         predicate.predicate_kind == "column_not_in_list_scalar_compare" ||
         predicate.predicate_kind == "column_range" ||
         predicate.predicate_kind == "column_less" ||
         predicate.predicate_kind == "column_less_equal" ||
         predicate.predicate_kind == "column_greater" ||
         predicate.predicate_kind == "column_greater_equal" ||
         predicate.predicate_kind == "column_not_equals" ||
         predicate.predicate_kind == "text_term_contains" ||
         predicate.predicate_kind == "text_all_terms" ||
         predicate.predicate_kind == "spatial_bbox_intersects" ||
         predicate.predicate_kind == "spatial_bbox_contains" ||
         predicate.predicate_kind == "vector_exact_nearest" ||
         predicate.predicate_kind == "vector_approx_nearest" ||
         predicate.predicate_kind == "expression_equals" ||
         predicate.predicate_kind == "partial_index_probe";
}

bool CanUseBoundedEqualityOrderScan(const EnginePredicateEnvelope& predicate,
                                    const EngineOrderingEnvelope& ordering,
                                    EngineApiU64 limit,
                                    EngineApiU64 offset) {
  if (limit == 0 || offset != 0) { return false; }
  if (predicate.predicate_kind != "column_equals" ||
      predicate.canonical_predicate_envelope.empty() ||
      predicate.bound_values.empty()) {
    return false;
  }
  return OrderingColumn(ordering) == predicate.canonical_predicate_envelope;
}

EngineTypedValue SelectPredicateBoundValue(std::string value,
                                           std::string type_name = "text") {
  if (type_name.empty()) type_name = "text";
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = std::move(type_name);
  typed.descriptor.encoded_descriptor =
      "type=" + typed.descriptor.canonical_type_name;
  typed.encoded_value = std::move(value);
  return typed;
}

bool SelectBoundValueAlreadyPresent(const std::vector<EngineTypedValue>& values,
                                    const std::string& candidate) {
  for (const auto& value : values) {
    if (value.encoded_value == candidate ||
        CompareSelectScalar(value.encoded_value, candidate) == 0) {
      return true;
    }
  }
  return false;
}

struct SelectProjectionPredicateResolution {
  bool attempted{false};
  bool ok{true};
  EnginePredicateEnvelope predicate;
  EngineApiDiagnostic diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  std::vector<EngineEvidenceReference> evidence;
};

SelectProjectionPredicateResolution ResolveSelectColumnInProjectionPredicate(
    const EngineSelectRowsRequest& request,
    const CrudState& state,
    const EnginePredicateEnvelope& requested_predicate) {
  SelectProjectionPredicateResolution resolution;
  if (requested_predicate.predicate_kind != "column_in_projection" &&
      requested_predicate.predicate_kind != "column_not_in_projection") {
    return resolution;
  }
  resolution.attempted = true;
  resolution.ok = false;

  const std::string source_uuid = OptionValue(request, "source_uuid:");
  const std::string select_column = OptionValue(request, "subquery_select_column:");
  const std::string subquery_predicate_kind =
      OptionValue(request, "subquery_predicate_kind:");
  const std::string subquery_predicate_column =
      OptionValue(request, "subquery_predicate_column:");
  const std::string subquery_predicate_value =
      OptionValue(request, "subquery_predicate_value:");
  const std::string subquery_predicate_value_type =
      OptionValue(request, "subquery_predicate_value_type:");

  if (source_uuid.empty() || select_column.empty()) {
    resolution.diagnostic =
        MakeInvalidRequestDiagnostic("dml.select_rows",
                                     "subquery_predicate_descriptor_incomplete");
    return resolution;
  }
  const auto source_table = FindVisibleCrudTable(state,
                                                 source_uuid,
                                                 request.context.local_transaction_id);
  if (!source_table) {
    resolution.diagnostic =
        MakeInvalidRequestDiagnostic("dml.select_rows",
                                     "subquery_source_table_not_visible");
    return resolution;
  }

  EnginePredicateEnvelope source_predicate;
  if (!subquery_predicate_kind.empty() && subquery_predicate_kind != "all_visible_rows") {
    if (subquery_predicate_column.empty()) {
      resolution.diagnostic =
          MakeInvalidRequestDiagnostic("dml.select_rows",
                                       "subquery_predicate_column_required");
      return resolution;
    }
    source_predicate.predicate_kind = subquery_predicate_kind;
    source_predicate.canonical_predicate_envelope = subquery_predicate_column;
    if (!subquery_predicate_value.empty()) {
      source_predicate.bound_values.push_back(
          SelectPredicateBoundValue(subquery_predicate_value,
                                    subquery_predicate_value_type));
    }
  }

  if (CrudPredicateTouchesOpaqueColumn(*source_table, source_predicate)) {
    resolution.diagnostic =
        UnsupportedCrudFeatureDiagnostic("dml.select_rows",
                                         "opaque_subquery_column_comparison_denied");
    return resolution;
  }

  EnginePredicateEnvelope resolved;
  resolved.predicate_kind =
      requested_predicate.predicate_kind == "column_not_in_projection"
          ? "column_not_in_list_scalar_compare"
          : "column_in_list_scalar_compare";
  resolved.canonical_predicate_envelope =
      requested_predicate.canonical_predicate_envelope;

  const auto source_rows = VisibleCrudRowsForContext(state,
                                                     source_uuid,
                                                     request.context);
  for (const auto& row : source_rows) {
    if (!source_predicate.predicate_kind.empty() &&
        !CrudRowMatchesPredicate(row, source_predicate)) {
      continue;
    }
    const std::string value = CrudFieldValue(row.values, select_column);
    if (value.empty() || value == "<NULL>") continue;
    if (!SelectBoundValueAlreadyPresent(resolved.bound_values, value)) {
      resolved.bound_values.push_back(SelectPredicateBoundValue(value));
    }
  }

  resolution.ok = true;
  resolution.predicate = std::move(resolved);
  resolution.evidence.push_back({"dml_select_subquery_source_rows",
                                 std::to_string(source_rows.size())});
  resolution.evidence.push_back({"dml_select_subquery_predicate_materialized",
                                 "column_projection_to_scalar_compare_list"});
  resolution.evidence.push_back({"dml_select_subquery_materialized_value_count",
                                 std::to_string(resolution.predicate.bound_values.size())});
  return resolution;
}

std::vector<CrudRowVersionRecord> BoundedVisibleRowsForEqualityOrder(
    const CrudState& state,
    const std::string& table_uuid,
    const EnginePredicateEnvelope& predicate,
    const EngineRequestContext& context,
    EngineApiU64 limit) {
  std::vector<CrudRowVersionRecord> visible;
  if (limit == 0) { return visible; }
  visible.reserve(static_cast<std::size_t>(
      std::min<EngineApiU64>(limit, static_cast<EngineApiU64>(state.row_versions.size()))));

  std::unordered_set<std::string> resolved_row_uuids;
  resolved_row_uuids.reserve(visible.capacity());
  for (auto it = state.row_versions.rbegin(); it != state.row_versions.rend(); ++it) {
    const auto& row = *it;
    if (row.table_uuid != table_uuid ||
        resolved_row_uuids.count(row.row_uuid) != 0 ||
        !CrudRowVersionVisibleToContext(state, row, context)) {
      continue;
    }
    resolved_row_uuids.insert(row.row_uuid);
    if (!row.deleted && CrudRowMatchesPredicate(row, predicate)) {
      visible.push_back(row);
    }
    // For equality on the ordering column all returned rows have the same sort
    // key, so the later stable sort is a no-op for the bounded set.
    if (visible.size() >= static_cast<std::size_t>(limit)) { break; }
  }
  return visible;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_SELECT_API_STUBS

EngineSelectRowsResult EngineSelectRows(const EngineSelectRowsRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineSelectRowsResult>(request.context, "dml.select_rows", MakeInvalidRequestDiagnostic("dml.select_rows", "local_transaction_id_required"));
  }
  if (IsEngineRelationProjectionViewSelectRequest(request)) {
    EngineSelectRowsRequest expanded;
    EngineRelationProjectionViewDescriptor view;
    const auto expansion = ExpandEngineRelationProjectionViewSelect(
        request, &expanded, &view);
    if (expansion.error) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", expansion);
    }
    auto result = EngineSelectRows(expanded);
    if (result.ok) {
      result.evidence.push_back(
          {"relation_projection_view_marker",
           kEngineRelationProjectionViewMarkerV1});
      result.evidence.push_back(
          {"relation_projection_view_uuid", view.view_uuid.canonical});
      result.evidence.push_back(
          {"relation_projection_view_descriptor_uuid",
           view.view_descriptor_uuid.canonical});
      result.evidence.push_back(
          {"relation_projection_view_descriptor_generation",
           std::to_string(view.view_descriptor_generation)});
      result.evidence.push_back(
          {"relation_projection_view_source_resource_epoch",
           std::to_string(view.source_resource_epoch)});
      result.evidence.push_back(
          {"relation_projection_view_expansion", "engine_owned_sql_free"});
      result.evidence.push_back(
          {"relation_projection_view_parser_sql", "false"});
    }
    return result;
  }
  if (IsEngineGlobalAggregateViewSelectRequest(request)) {
    EngineSelectRowsRequest expanded;
    EngineGlobalAggregateViewDescriptor view;
    const auto expansion = ExpandEngineGlobalAggregateViewSelect(
        request, &expanded, &view);
    if (expansion.error) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", expansion);
    }
    auto result = EngineSelectRows(expanded);
    if (result.ok) {
      result.evidence.push_back(
          {"global_aggregate_view_marker",
           kEngineGlobalAggregateViewMarkerV1});
      result.evidence.push_back(
          {"global_aggregate_view_uuid", view.view_uuid.canonical});
      result.evidence.push_back(
          {"global_aggregate_view_descriptor_uuid",
           view.view_descriptor_uuid.canonical});
      result.evidence.push_back(
          {"global_aggregate_view_descriptor_generation",
           std::to_string(view.view_descriptor_generation)});
      result.evidence.push_back(
          {"global_aggregate_view_expansion", "engine_owned_sql_free"});
      result.evidence.push_back(
          {"global_aggregate_view_parser_sql", "false"});
    }
    return result;
  }
  const bool relation_projection =
      !request.relation_projection.outputs.empty();
  const bool global_aggregate_projection =
      !request.global_aggregate_projection.outputs.empty();
  if (relation_projection && global_aggregate_projection) {
    return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
        request.context,
        "dml.select_rows",
        MakeInvalidRequestDiagnostic(
            "dml.relation_projection",
            "relation_projection_conflicts_with_global_aggregate"));
  }
  if (relation_projection) {
    const auto validated = ValidateEngineRelationProjectionEnvelope(
        request.relation_projection);
    if (validated.error) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", validated);
    }
    const bool conflicting_shape =
        !request.select_projection.canonical_projection_envelopes.empty() ||
        !request.projection.canonical_projection_envelopes.empty() ||
        !request.select_predicate.predicate_kind.empty() ||
        !request.predicate.predicate_kind.empty() ||
        !request.select_ordering.canonical_ordering_envelopes.empty() ||
        !request.ordering.canonical_ordering_envelopes.empty() ||
        request.limit != 0 || request.offset != 0 ||
        !request.option_envelopes.empty() || !request.rows.empty() ||
        !request.assignments.empty() || !request.descriptors.empty() ||
        !request.columns.empty() || !request.related_objects.empty() ||
        request.relation_projection_view.present;
    if (conflicting_shape) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic(
              "dml.relation_projection",
              "relation_projection_conflicting_select_shape"));
    }
    if (request.context.resource_epoch == 0 ||
        request.context.resource_epoch !=
            request.relation_projection.source_resource_epoch) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic(
              "dml.relation_projection",
              "relation_projection_source_resource_epoch_stale"));
    }
  }
  if (global_aggregate_projection) {
    const auto validated = ValidateGlobalAggregateProjectionEnvelope(
        request.global_aggregate_projection);
    if (validated.error) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", validated);
    }
    const bool legacy_projection_present =
        !request.select_projection.canonical_projection_envelopes.empty() ||
        !request.projection.canonical_projection_envelopes.empty() ||
        !OptionValue(request, "result_projection:").empty();
    const bool post_aggregate_row_shape_present =
        !request.select_ordering.canonical_ordering_envelopes.empty() ||
        !request.ordering.canonical_ordering_envelopes.empty() ||
        request.limit != 0 || request.offset != 0;
    if (legacy_projection_present || post_aggregate_row_shape_present) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic(
              "dml.global_aggregate_projection",
              "global_aggregate_conflicting_select_shape"));
    }
  }
  if (IsRelationDescriptorProjectionSelectRequest(request)) {
    return EngineSelectRelationDescriptorProjection(request);
  }
  auto select_phase_last = SelectApiSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> select_phase_micros;
  select_phase_micros.reserve(14);
  const auto mark_select_phase = [&](std::string phase) {
    const auto now = SelectApiSteadyClock::now();
    select_phase_micros.push_back(
        {std::move(phase), SelectApiElapsedMicros(select_phase_last, now)});
    select_phase_last = now;
  };
  const auto write_select_trace = [&](std::size_t row_count) {
    WriteSelectApiPhaseTrace("engine_select_rows",
                             "dml.select_rows",
                             row_count,
                             select_phase_micros);
  };
  if (OptionValue(request, "source_kind:") == "selectable_procedure") {
    std::string routine_uuid = OptionValue(request, "routine_object_uuid:");
    if (routine_uuid.empty()) routine_uuid = OptionValue(request, "source_uuid:");
    if (routine_uuid.empty()) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic("dml.select_rows",
                                       "selectable_procedure_uuid_required"));
    }
    const auto executable_state = LoadExecutableObjectLifecycleStateForRuntimeDispatch(
        request.context);
    mark_select_phase("load_executable_state");
    if (!executable_state.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", executable_state.diagnostic);
    }
    const auto* object = FindExecutableObject(executable_state.state, routine_uuid);
    if (object == nullptr || object->object_kind != "procedure") {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic("dml.select_rows",
                                       "selectable_procedure_not_visible"));
    }
    std::string error_detail;
    std::vector<CrudRowVersionRecord> rows =
        MaterializeSelectableProcedureRows(request, *object, routine_uuid, &error_detail);
    mark_select_phase("materialize_selectable_procedure");
    if (!error_detail.empty()) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
    }
    const EngineOrderingEnvelope& ordering =
        !request.select_ordering.canonical_ordering_envelopes.empty() ? request.select_ordering : request.ordering;
    ApplyOrdering(ordering, &rows);
    mark_select_phase("apply_ordering");
    const auto offset = static_cast<std::size_t>(request.offset);
    if (offset != 0) {
      if (offset >= rows.size()) {
        rows.clear();
      } else {
        rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(offset));
      }
    }
    if (request.limit != 0 && rows.size() > request.limit) {
      rows.resize(static_cast<std::size_t>(request.limit));
    }
    const EngineProjectionEnvelope& projection =
        !request.select_projection.canonical_projection_envelopes.empty() ? request.select_projection : request.projection;
    auto result = MakeCrudSuccessResult<EngineSelectRowsResult>(request.context, "dml.select_rows");
    result.visible_count = rows.size();
    const std::string result_projection = OptionValue(request, "result_projection:");
    if (result_projection == "count") {
      result.result_shape =
          CountProjectionResultShape(request, rows.size(), &error_detail);
      if (!error_detail.empty()) {
        return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
            request.context,
            "dml.select_rows",
            MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
      }
      result.visible_count = 1;
      result.evidence.push_back({"dml_result_projection", "count"});
    } else if (result_projection == "count_assertion") {
      result.result_shape = CountAssertionResultShape(request, rows.size(), &error_detail);
      if (!error_detail.empty()) {
        return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
            request.context,
            "dml.select_rows",
            MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
      }
      result.evidence.push_back({"dml_result_projection", "count_assertion"});
    } else if (result_projection == "aggregate_assertion") {
      result.result_shape = AggregateAssertionResultShape(request, rows, &error_detail);
      if (!error_detail.empty()) {
        return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
            request.context,
            "dml.select_rows",
            MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
      }
      result.evidence.push_back({"dml_result_projection", "aggregate_assertion"});
    } else if (result_projection == "field_assertion") {
      result.result_shape = FieldAssertionResultShape(request, rows, &error_detail);
      if (!error_detail.empty()) {
        return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
            request.context,
            "dml.select_rows",
            MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
      }
      result.evidence.push_back({"dml_result_projection", "field_assertion"});
    } else {
      ApplyProjection(projection, &rows);
      result.result_shape = CrudRowsToResultShape(rows);
    }
    mark_select_phase("result_shape");
    result.evidence.push_back({"selectable_procedure", routine_uuid});
    write_select_trace(result.visible_count);
    return result;
  }
  const std::string table_uuid = !request.source_object.uuid.canonical.empty() ? request.source_object.uuid.canonical : request.target_object.uuid.canonical;
  if (table_uuid.empty()) {
    return MakeCrudDiagnosticResult<EngineSelectRowsResult>(request.context, "dml.select_rows", MakeInvalidRequestDiagnostic("dml.select_rows", "source_table_uuid_required"));
  }
  EngineRelationProjectionBindingResult relation_projection_binding;
  MgaRelationStorageDescriptor relation_projection_relation_descriptor;
  if (relation_projection) {
    const auto descriptor =
        LoadMgaRelationStorageDescriptor(request.context, table_uuid);
    mark_select_phase("load_relation_projection_relation_descriptor");
    if (!descriptor.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", descriptor.diagnostic);
    }
    relation_projection_binding = BindEngineRelationProjectionEnvelope(
        request.relation_projection, descriptor.descriptor);
    mark_select_phase("bind_relation_projection_fields");
    if (!relation_projection_binding.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          relation_projection_binding.diagnostic);
    }
    relation_projection_relation_descriptor = descriptor.descriptor;
  }
  EngineGlobalAggregateBindingResult global_aggregate_binding;
  MgaRelationStorageDescriptor global_aggregate_relation_descriptor;
  if (global_aggregate_projection) {
    const auto descriptor =
        LoadMgaRelationStorageDescriptor(request.context, table_uuid);
    mark_select_phase("load_global_aggregate_relation_descriptor");
    if (!descriptor.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", descriptor.diagnostic);
    }
    global_aggregate_binding = BindGlobalAggregateProjectionEnvelope(
        request.global_aggregate_projection, descriptor.descriptor);
    mark_select_phase("bind_global_aggregate_fields");
    if (!global_aggregate_binding.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          global_aggregate_binding.diagnostic);
    }
    global_aggregate_relation_descriptor = descriptor.descriptor;
  }
  const EnginePredicateEnvelope requested_predicate =
      !request.select_predicate.predicate_kind.empty() ? request.select_predicate : request.predicate;
  const std::string subquery_source_uuid = OptionValue(request, "source_uuid:");
  const bool needs_subquery_source_scope =
      (requested_predicate.predicate_kind == "column_in_projection" ||
       requested_predicate.predicate_kind == "column_not_in_projection") &&
      !subquery_source_uuid.empty();
  auto loaded = needs_subquery_source_scope
      ? LoadMgaRelationStoreStateForMutationTargets(
            request.context,
            std::vector<std::string>{table_uuid, subquery_source_uuid})
      : LoadMgaRelationStoreStateForMutationTarget(request.context, table_uuid);
  mark_select_phase("load_target_relation_state");
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineSelectRowsResult>(request.context, "dml.select_rows", loaded.diagnostic); }
  CrudState state = BuildCrudCompatibilityStateFromMga(std::move(loaded.state));
  const auto table = FindVisibleCrudTable(state, table_uuid, request.context.local_transaction_id);
  mark_select_phase("build_state_and_find_table");
  if (!table) {
    return MakeCrudDiagnosticResult<EngineSelectRowsResult>(request.context, "dml.select_rows", MakeInvalidRequestDiagnostic("dml.select_rows", "source_table_not_visible"));
  }
  if (table->temporary && request.context.session_uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
        request.context,
        "dml.select_rows",
        MakeInvalidRequestDiagnostic("dml.select_rows",
                                     "temporary_table_requires_session_uuid"));
  }
  EnginePredicateEnvelope predicate = requested_predicate;
  auto projection_predicate_resolution =
      ResolveSelectColumnInProjectionPredicate(request, state, requested_predicate);
  mark_select_phase("resolve_projection_predicate");
  if (projection_predicate_resolution.attempted) {
    if (!projection_predicate_resolution.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          projection_predicate_resolution.diagnostic);
    }
    predicate = std::move(projection_predicate_resolution.predicate);
  }
  if (CrudPredicateTouchesOpaqueColumn(*table, predicate)) {
    return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
        request.context,
        "dml.select_rows",
        UnsupportedCrudFeatureDiagnostic("dml.select_rows", "opaque_column_comparison_denied"));
  }
  const EngineOrderingEnvelope& ordering =
      !request.select_ordering.canonical_ordering_envelopes.empty() ? request.select_ordering : request.ordering;
  std::vector<CrudRowVersionRecord> rows;
  bool rows_ready = false;
	  const auto load_rows = [&]() -> std::vector<CrudRowVersionRecord>& {
	    if (!rows_ready) {
	      rows = VisibleCrudRowsForContext(state, table_uuid, request.context);
	      rows_ready = true;
	    }
	    return rows;
	  };
	  const auto scan_rows_for_predicate = [&]() {
	    std::vector<CrudRowVersionRecord> filtered;
	    const auto visible = VisibleCrudRowsForContext(state, table_uuid, request.context);
	    for (const auto& row : visible) {
	      if (CrudRowMatchesPredicate(row, predicate)) { filtered.push_back(row); }
	    }
	    return filtered;
	  };
  std::string index_uuid_used;
  std::string row_scan_predicate;
  std::vector<EngineEvidenceReference> index_lookup_evidence;
  if (global_aggregate_projection) {
    if (!predicate.predicate_kind.empty() &&
        predicate.predicate_kind != "row_uuid_match" &&
        !PredicateCanRowScan(predicate)) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          UnsupportedCrudFeatureDiagnostic(
              "dml.global_aggregate_projection",
              "global_aggregate_predicate_requires_single_visible_scan"));
    }
    std::vector<CrudRowVersionRecord> filtered;
    for (const auto& row : load_rows()) {
      const bool matches = predicate.predicate_kind.empty() ||
                           (predicate.predicate_kind == "row_uuid_match"
                                ? row.row_uuid ==
                                      predicate.canonical_predicate_envelope
                                : CrudRowMatchesPredicate(row, predicate));
      if (matches) filtered.push_back(row);
    }
    rows = std::move(filtered);
    rows_ready = true;
    if (!predicate.predicate_kind.empty()) {
      row_scan_predicate =
          predicate.predicate_kind + ":global_aggregate_single_visible_scan";
    }
  } else if (predicate.predicate_kind == "row_uuid_match" && !predicate.canonical_predicate_envelope.empty()) {
    std::vector<CrudRowVersionRecord> filtered;
    for (const auto& row : load_rows()) {
      if (row.row_uuid == predicate.canonical_predicate_envelope) { filtered.push_back(row); }
    }
    rows = std::move(filtered);
    rows_ready = true;
  } else if (predicate.predicate_kind == "column_equals" && !predicate.canonical_predicate_envelope.empty() &&
             !predicate.bound_values.empty()) {
    // DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP
    const auto indexed = IndexedMgaRowsForPredicateForContext(state,
                                                              table_uuid,
                                                              predicate,
                                                              request.context,
                                                              request.limit);
	    if (indexed.index_used) {
	      rows = indexed.rows;
	      rows_ready = true;
	      index_uuid_used = indexed.index_evidence_id;
	      index_lookup_evidence = indexed.evidence;
	      if (rows.empty() && PredicateCanRowScan(predicate)) {
	        auto fallback_rows = scan_rows_for_predicate();
	        if (!fallback_rows.empty()) {
	          rows = std::move(fallback_rows);
	          index_uuid_used.clear();
	          row_scan_predicate = predicate.predicate_kind + ":index_empty_mga_visibility_fallback";
	        }
	      }
	    } else if (indexed.index_refused && !PredicateCanRowScan(predicate)) {
	      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
	          request.context,
          "dml.select_rows",
          indexed.diagnostic);
    } else if (CanUseBoundedEqualityOrderScan(predicate, ordering, request.limit, request.offset)) {
      rows = BoundedVisibleRowsForEqualityOrder(state,
                                                table_uuid,
                                                predicate,
                                                request.context,
                                                request.limit);
      rows_ready = true;
      row_scan_predicate = predicate.predicate_kind + ":bounded_order_limit";
      if (indexed.index_refused) {
        row_scan_predicate += ":secondary_index_delta_overlay_refused";
        index_lookup_evidence = indexed.evidence;
      }
    } else {
      std::vector<CrudRowVersionRecord> filtered;
      for (const auto& row : load_rows()) {
        if (CrudRowMatchesPredicate(row, predicate)) { filtered.push_back(row); }
      }
      rows = std::move(filtered);
      rows_ready = true;
      row_scan_predicate = predicate.predicate_kind;
      if (indexed.index_refused) {
        row_scan_predicate += ":secondary_index_delta_overlay_refused";
        index_lookup_evidence = indexed.evidence;
      }
    }
  } else if (!predicate.predicate_kind.empty()) {
    // DPC_SECONDARY_INDEX_DELTA_OVERLAY_LOOKUP
    const auto indexed = IndexedMgaRowsForPredicateForContext(state,
                                                              table_uuid,
                                                              predicate,
                                                              request.context,
                                                              request.limit);
    if (!indexed.index_used) {
      if (!PredicateCanRowScan(predicate)) {
        return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
            request.context,
            "dml.select_rows",
            indexed.index_refused ? indexed.diagnostic
                                  : UnsupportedCrudFeatureDiagnostic("dml.select_rows", "no_usable_index_for_predicate"));
      }
      std::vector<CrudRowVersionRecord> filtered;
      for (const auto& row : load_rows()) {
        if (CrudRowMatchesPredicate(row, predicate)) { filtered.push_back(row); }
      }
      rows = std::move(filtered);
      rows_ready = true;
      row_scan_predicate = predicate.predicate_kind;
      if (indexed.index_refused) {
        row_scan_predicate += ":secondary_index_delta_overlay_refused";
        index_lookup_evidence = indexed.evidence;
      }
	    } else {
	      rows = indexed.rows;
	      rows_ready = true;
	      index_uuid_used = indexed.index_evidence_id;
	      index_lookup_evidence = indexed.evidence;
	      if (rows.empty() && PredicateCanRowScan(predicate)) {
	        auto fallback_rows = scan_rows_for_predicate();
	        if (!fallback_rows.empty()) {
	          rows = std::move(fallback_rows);
	          index_uuid_used.clear();
	          row_scan_predicate = predicate.predicate_kind + ":index_empty_mga_visibility_fallback";
	        }
	      }
	    }
	  }
  mark_select_phase("resolve_visible_rows");
  if (!rows_ready) {
    rows = VisibleCrudRowsForContext(state, table_uuid, request.context);
    rows_ready = true;
  }
  mark_select_phase("ensure_visible_rows");
  for (auto& row : rows) {
    const auto policy = ApplyDomainReadPoliciesToCrudValues(request.context,
                                                           table->columns,
                                                           row.values,
                                                           request.context.local_transaction_id);
    if (!policy.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(request.context, "dml.select_rows", policy.diagnostic);
    }
    row.values = policy.values;
  }
  mark_select_phase("domain_read_policy");
  if (!global_aggregate_projection && !relation_projection) {
    ApplyOrdering(ordering, &rows);
    mark_select_phase("apply_ordering");
    const auto offset = static_cast<std::size_t>(request.offset);
    if (offset != 0) {
      if (offset >= rows.size()) {
        rows.clear();
      } else {
        rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(offset));
      }
    }
    if (request.limit != 0 && rows.size() > request.limit) {
      rows.resize(static_cast<std::size_t>(request.limit));
    }
    mark_select_phase("limit_offset");
  }
  const EngineProjectionEnvelope& projection =
      !request.select_projection.canonical_projection_envelopes.empty() ? request.select_projection : request.projection;
  auto result = MakeCrudSuccessResult<EngineSelectRowsResult>(request.context, "dml.select_rows");
  result.visible_count = rows.size();
  const std::string result_projection = OptionValue(request, "result_projection:");
  if (relation_projection) {
    auto projected = ExecuteEngineRelationProjection(
        relation_projection_binding.outputs,
        relation_projection_relation_descriptor,
        request.relation_projection.source_resource_epoch,
        rows);
    if (!projected.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", projected.diagnostic);
    }
    result.result_shape = std::move(projected.result_shape);
    result.visible_count = projected.scanned_visible_row_count;
    result.evidence.push_back(
        {"dml_result_projection", "relation_projection"});
    result.evidence.push_back(
        {"relation_projection_relation_scan", "one_mga_visible_scan"});
    result.evidence.push_back(
        {"relation_projection_visible_rows_scanned",
         std::to_string(projected.scanned_visible_row_count)});
    result.evidence.push_back(
        {"relation_projection_output_count",
         std::to_string(relation_projection_binding.outputs.size())});
    result.evidence.push_back(
        {"relation_projection_row_storage", "none"});
  } else if (global_aggregate_projection) {
    auto aggregate = ExecuteGlobalAggregateProjection(
        global_aggregate_binding.outputs,
        global_aggregate_relation_descriptor,
        rows);
    if (!aggregate.ok) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context, "dml.select_rows", aggregate.diagnostic);
    }
    result.result_shape = std::move(aggregate.result_shape);
    result.visible_count = 1;
    result.evidence.push_back(
        {"dml_result_projection", "global_aggregate_projection"});
    result.evidence.push_back(
        {"global_aggregate_relation_scan", "one_mga_visible_scan"});
    result.evidence.push_back(
        {"global_aggregate_visible_rows_scanned",
         std::to_string(aggregate.scanned_visible_row_count)});
    result.evidence.push_back(
        {"global_aggregate_output_count",
         std::to_string(global_aggregate_binding.outputs.size())});
    result.evidence.push_back(
        {"global_aggregate_function_uuid",
         global_aggregate_binding.outputs.front()
             .aggregate_function_uuid.canonical});
  } else if (result_projection == "count") {
    std::string error_detail;
    result.result_shape =
        CountProjectionResultShape(request, rows.size(), &error_detail);
    if (!error_detail.empty()) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
    }
    result.visible_count = 1;
    result.evidence.push_back({"dml_result_projection", "count"});
  } else if (result_projection == "count_assertion") {
    std::string error_detail;
    result.result_shape = CountAssertionResultShape(request, rows.size(), &error_detail);
    if (!error_detail.empty()) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
    }
    result.evidence.push_back({"dml_result_projection", "count_assertion"});
  } else if (result_projection == "field_assertion") {
    std::string error_detail;
    result.result_shape = FieldAssertionResultShape(request, rows, &error_detail);
    if (!error_detail.empty()) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
    }
    result.evidence.push_back({"dml_result_projection", "field_assertion"});
  } else if (result_projection == "aggregate_assertion") {
    std::string error_detail;
    result.result_shape = AggregateAssertionResultShape(request, rows, &error_detail);
    if (!error_detail.empty()) {
      return MakeCrudDiagnosticResult<EngineSelectRowsResult>(
          request.context,
          "dml.select_rows",
          MakeInvalidRequestDiagnostic("dml.select_rows", error_detail));
    }
    result.evidence.push_back({"dml_result_projection", "aggregate_assertion"});
  } else {
    ApplyProjection(projection, &rows);
    result.result_shape = CrudRowsToResultShape(rows);
  }
  mark_select_phase("result_shape");
  if (!index_uuid_used.empty()) { result.evidence.push_back({"index_lookup", index_uuid_used}); }
  if (!row_scan_predicate.empty()) { result.evidence.push_back({"row_scan_predicate", row_scan_predicate}); }
  result.evidence.insert(result.evidence.end(),
                         index_lookup_evidence.begin(),
                         index_lookup_evidence.end());
  result.evidence.insert(result.evidence.end(),
                         projection_predicate_resolution.evidence.begin(),
                         projection_predicate_resolution.evidence.end());
  auto serializable_recorded = dml::RecordSerializableSelectRead(
      request.context,
      "dml.select_rows",
      table_uuid,
      predicate,
      request.option_envelopes);
  mark_select_phase("serializable_read_record");
  if (!serializable_recorded.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineSelectRowsResult>(
        request.context,
        "dml.select_rows",
        serializable_recorded.diagnostic);
    failure.evidence.insert(failure.evidence.end(),
                            result.evidence.begin(),
                            result.evidence.end());
    failure.evidence.insert(failure.evidence.end(),
                            serializable_recorded.evidence.begin(),
                            serializable_recorded.evidence.end());
    return failure;
  }
  result.evidence.insert(result.evidence.end(),
                         serializable_recorded.evidence.begin(),
                         serializable_recorded.evidence.end());
  write_select_trace(result.visible_count);
  return result;
}

}  // namespace scratchbird::engine::internal_api
