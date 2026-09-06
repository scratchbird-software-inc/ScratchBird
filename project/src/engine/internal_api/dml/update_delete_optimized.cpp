// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_delete_optimized.hpp"

#include "crud_support/crud_store.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/delete_batch.hpp"
#include "dml/dml_executable_trigger_runtime.hpp"
#include "dml/dml_row_locator_stream.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "dml/index_apply_locality_bridge.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "dml/update_batch.hpp"
#include "dml/update_datatype_operator_authority_provider.hpp"
#include "dml/update_durable_operation_authority_provider.hpp"
#include "dml/update_immutable_authority_provider.hpp"
#include "dml/update_policy_catalog_authority_provider.hpp"
#include "dml/update_statement_mga_authority_provider.hpp"
#include "dml/transactional_relation_store.hpp"
#include "dml/transactional_index_provider.hpp"
#include "dml/write_result_policy.hpp"
#include "datatype_catalog_manifest.hpp"
#include "domain_support/domain_store.hpp"
#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/dml_summary_counters.hpp"
#include "physical_plan.hpp"
#include "relational_planner.hpp"
#include "row_version.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "transaction_inventory.hpp"
#include "typed_update_carrier_codec.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace mga = scratchbird::transaction::mga;
namespace storage_db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
namespace update_wire = scratchbird::wire;

using UpdateDeleteSteadyClock = std::chrono::steady_clock;

std::uint64_t UpdateDeleteElapsedMicros(
    UpdateDeleteSteadyClock::time_point begin,
    UpdateDeleteSteadyClock::time_point end = UpdateDeleteSteadyClock::now()) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

void WriteUpdateDeletePhaseTrace(
    std::string_view layer,
    std::string_view operation,
    std::size_t row_count,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_UPDATE_DELETE_PHASE_TRACE_FILE");
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

void AddMutationOptimizerEvidence(const char* mutation_kind,
                                  bool transaction_context_present,
                                  bool visibility_proven,
                                  std::vector<EngineEvidenceReference>* evidence) {
  const auto decision = opt::PlanLocalMutation(mutation_kind, transaction_context_present, visibility_proven);
  evidence->push_back({"optimizer_mutation_kind", mutation_kind});
  if (!decision.ok) {
    const std::string detail = decision.diagnostics.empty() ? "mutation_plan_rejected" : decision.diagnostics.front();
    evidence->push_back({"optimizer_plan_rejected", detail});
    return;
  }
  evidence->push_back({"optimizer_selected_access", plan::PhysicalAccessKindName(decision.access_kind)});
  evidence->push_back({"optimizer_executor_capability", opt::RequiredExecutorCapabilityForAccessKind(decision.access_kind)});
}

EngineUpdateRowsResult AllocationFailureResult(const EngineRequestContext& context,
                                               const DmlPageAllocationRuntimeResult& allocation) {
  auto failure = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
      context,
      "dml.update_rows",
      allocation.diagnostic);
  AddDmlPageAllocationRuntimeEvidence(allocation, &failure);
  return failure;
}

struct StagedUpdateRow {
  CrudRowVersionRecord row_record;
  CrudRowVersionRecord original_row;
  std::vector<std::pair<std::string, std::string>> logical_values;
  mga::HotStableRowHeadDecisionResult hot_plus_decision;
  struct IndexKeyState {
    bool materialized = false;
    bool keys_changed = false;
    std::vector<std::string> old_keys;
    std::vector<std::string> new_keys;
  };
  std::vector<IndexKeyState> index_key_states;
  std::size_t encoded_bytes = 0;
  bool toast_required = false;
};

struct StagedDeleteRow {
  CrudRowVersionRecord row_record;
  CrudRowVersionRecord original_row;
};

struct UpdateTargetCandidateStream {
  DmlTargetAccessPlan plan;
  std::vector<CrudRowVersionRecord> rows;
  std::vector<EngineEvidenceReference> evidence;
  EngineApiDiagnostic diagnostic;
  bool rows_ready = false;
  bool fail_closed = false;
};

struct DeleteTargetCandidateStream {
  DmlTargetAccessPlan plan;
  std::vector<CrudRowVersionRecord> rows;
  std::vector<EngineEvidenceReference> evidence;
  EngineApiDiagnostic diagnostic;
  bool rows_ready = false;
  bool fail_closed = false;
};

struct UpdateRowEvidenceCompactor {
  bool enabled = false;
  EngineApiU64 input_row_count = 0;
  EngineApiU64 total_compacted_entries = 0;
  std::unordered_map<std::string, EngineApiU64> counts_by_kind;

  void AppendOrCompact(const std::vector<EngineEvidenceReference>& evidence,
                       std::vector<EngineEvidenceReference>* direct_target) {
    if (!enabled) {
      if (direct_target != nullptr) {
        direct_target->insert(direct_target->end(), evidence.begin(), evidence.end());
      }
      return;
    }
    for (const auto& entry : evidence) {
      ++total_compacted_entries;
      ++counts_by_kind[entry.evidence_kind];
    }
  }

  void PushOrCompact(EngineEvidenceReference evidence,
                     std::vector<EngineEvidenceReference>* direct_target) {
    if (!enabled) {
      if (direct_target != nullptr) {
        direct_target->push_back(std::move(evidence));
      }
      return;
    }
    ++total_compacted_entries;
    ++counts_by_kind[evidence.evidence_kind];
  }

  void AddSummaryEvidence(std::vector<EngineEvidenceReference>* direct_target) const {
    if (!enabled || direct_target == nullptr) {
      return;
    }
    direct_target->push_back({"update_row_evidence_compacted", "true"});
    direct_target->push_back({"update_row_evidence_input_rows",
                              std::to_string(input_row_count)});
    direct_target->push_back({"update_row_evidence_entry_count",
                              std::to_string(total_compacted_entries)});
    for (const auto& [kind, count] : counts_by_kind) {
      direct_target->push_back({"update_row_evidence_count." + kind,
                                std::to_string(count)});
    }
  }
};

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0, prefix.size()) == prefix;
}

bool UpdateOptionEnabled(const EngineUpdateRowsRequest& request,
                         std::string_view option) {
  return std::find(request.option_envelopes.begin(),
                   request.option_envelopes.end(),
                   option) != request.option_envelopes.end();
}

std::uint64_t UpdateOptionU64(const EngineUpdateRowsRequest& request,
                              std::string_view prefix,
                              std::uint64_t fallback) {
  for (const auto& option : request.option_envelopes) {
    if (!StartsWith(option, prefix)) {
      continue;
    }
    try {
      return static_cast<std::uint64_t>(std::stoull(option.substr(prefix.size())));
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

std::string UpdateOptionText(const EngineUpdateRowsRequest& request,
                             std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool UpdateMutationWindowActive(const EngineUpdateRowsRequest& request) {
  return request.limit != 0 || request.offset != 0;
}

bool DeleteMutationWindowActive(const EngineDeleteRowsRequest& request) {
  return request.limit != 0 || request.offset != 0;
}

EngineApiDiagnostic ValidateMutationRowWindow(std::string_view operation,
                                              EngineApiU64 limit,
                                              EngineApiU64 offset,
                                              bool conflicts_with_batch_limit) {
  if (offset != 0 && limit == 0) {
    return MakeInvalidRequestDiagnostic(std::string(operation),
                                        "mutation_row_window_offset_requires_limit");
  }
  if ((limit != 0 || offset != 0) && conflicts_with_batch_limit) {
    return MakeInvalidRequestDiagnostic(
        std::string(operation),
        "mutation_row_window_conflicts_with_batch_limit");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

void NormalizeUpdatePredicateFromLoweredOptions(EngineUpdateRowsRequest* request) {
  if (request == nullptr) { return; }
  const std::string predicate_kind = UpdateOptionText(*request, "predicate_kind:");
  const std::string predicate_column = UpdateOptionText(*request, "predicate_column:");
  const std::string predicate_value = UpdateOptionText(*request, "predicate_value:");
  const std::string predicate_value_type =
      UpdateOptionText(*request, "predicate_value_type:");
  if (!predicate_kind.empty()) {
    request->update_predicate.predicate_kind = predicate_kind;
  }
  if (!predicate_column.empty()) {
    request->update_predicate.canonical_predicate_envelope = predicate_column;
  }
  if (request->update_predicate.bound_values.empty() && !predicate_value.empty()) {
    EngineTypedValue typed;
    typed.descriptor.descriptor_kind = "scalar";
    typed.descriptor.canonical_type_name =
        predicate_value_type.empty() ? "text" : predicate_value_type;
    typed.descriptor.encoded_descriptor =
        "type=" + typed.descriptor.canonical_type_name;
    typed.is_null = predicate_value == "<NULL>";
    typed.encoded_value = typed.is_null ? std::string{} : predicate_value;
    if (typed.is_null) {
      typed.setState(EngineValueState::sql_null);
    }
    request->update_predicate.bound_values.push_back(std::move(typed));
  }
}

std::vector<std::string> SplitText(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

struct UpdateAssignmentExpression {
  std::string target_column;
  std::string source_column;
  std::string operation;
  std::string literal_value;
  std::string literal_type;
  std::vector<std::pair<long double, std::string>> case_ge_thresholds;
  std::vector<std::pair<long long, std::string>> case_ge_integer_thresholds;
  std::optional<std::string> case_fallback;
  std::size_t cached_source_index = std::string::npos;
  std::size_t cached_target_index = std::string::npos;
  bool case_ge_thresholds_integral = false;
};

bool ParseLongDoubleValue(const std::string& value, long double* out);
bool ParseLongLongValue(const std::string& value, long long* out);
const std::string* CachedCrudFieldValuePtr(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& field,
    std::size_t* cached_index);
std::string* CachedMutableCrudFieldValuePtr(
    std::vector<std::pair<std::string, std::string>>* values,
    const std::string& field,
    std::size_t* cached_index);

bool CompileCaseGeThresholds(UpdateAssignmentExpression* expression) {
  if (expression == nullptr) { return false; }
  expression->case_ge_thresholds.clear();
  expression->case_ge_integer_thresholds.clear();
  expression->case_fallback.reset();
  expression->case_ge_thresholds_integral = true;
  for (const auto& term : SplitText(expression->literal_value, ',')) {
    const auto separator = term.find('=');
    if (separator == std::string::npos) { return false; }
    const std::string left = term.substr(0, separator);
    const std::string right = term.substr(separator + 1);
    if (left == "else") {
      expression->case_fallback = right;
      continue;
    }
    long double threshold = 0.0;
    if (!ParseLongDoubleValue(left, &threshold)) { return false; }
    expression->case_ge_thresholds.push_back({threshold, right});
    long long integer_threshold = 0;
    if (ParseLongLongValue(left, &integer_threshold)) {
      expression->case_ge_integer_thresholds.push_back(
          {integer_threshold, right});
    } else {
      expression->case_ge_thresholds_integral = false;
    }
  }
  if (expression->case_ge_integer_thresholds.size() !=
      expression->case_ge_thresholds.size()) {
    expression->case_ge_thresholds_integral = false;
    expression->case_ge_integer_thresholds.clear();
  }
  return !expression->case_ge_thresholds.empty() ||
         expression->case_fallback.has_value();
}

std::vector<UpdateAssignmentExpression> ParseUpdateAssignmentPlan(
    const EngineUpdateRowsRequest& request,
    bool* invalid) {
  if (invalid != nullptr) { *invalid = false; }
  const std::string plan = UpdateOptionText(request, "assignment_plan:");
  if (plan.empty()) { return {}; }

  std::vector<UpdateAssignmentExpression> expressions;
  for (const auto& item : SplitText(plan, ';')) {
    const auto parts = SplitText(item, '|');
    if (parts.size() != 5 || parts[0].empty() || parts[2].empty()) {
      if (invalid != nullptr) { *invalid = true; }
      return {};
    }
    UpdateAssignmentExpression expression;
    expression.target_column = parts[0];
    expression.source_column = parts[1];
    expression.operation = parts[2];
    expression.literal_value = parts[3];
    expression.literal_type = parts[4];
    if (expression.operation != "literal" &&
        expression.operation != "add" &&
        expression.operation != "subtract" &&
        expression.operation != "multiply" &&
        expression.operation != "case_ge_thresholds" &&
        expression.operation != "concat" &&
        expression.operation != "copy_column") {
      if (invalid != nullptr) { *invalid = true; }
      return {};
    }
    if (expression.operation != "literal" && expression.source_column.empty()) {
      if (invalid != nullptr) { *invalid = true; }
      return {};
    }
    if (expression.operation == "case_ge_thresholds" &&
        !CompileCaseGeThresholds(&expression)) {
      if (invalid != nullptr) { *invalid = true; }
      return {};
    }
    expressions.push_back(std::move(expression));
  }
  return expressions;
}

std::vector<std::string> UpdateAssignedColumns(
    const EngineUpdateRowsRequest& request,
    const std::vector<UpdateAssignmentExpression>& expressions) {
  std::set<std::string> assigned;
  for (const auto& [field, typed] : request.assignments) {
    (void)typed;
    if (!field.empty()) { assigned.insert(field); }
  }
  for (const auto& expression : expressions) {
    if (!expression.target_column.empty()) {
      assigned.insert(expression.target_column);
    }
  }
  return {assigned.begin(), assigned.end()};
}

bool UpdateTouchesDomainColumns(const CrudTableRecord& table,
                                const std::vector<std::string>& assigned_columns) {
  if (assigned_columns.empty()) { return true; }
  const std::set<std::string> assigned(assigned_columns.begin(), assigned_columns.end());
  for (const auto& [column_name, descriptor] : table.columns) {
    if (assigned.find(column_name) == assigned.end()) { continue; }
    if (!DomainUuidFromColumnDescriptor(descriptor).empty()) { return true; }
  }
  return false;
}

std::string EvaluateCaseGeThresholds(const std::string& source_value,
                                     const UpdateAssignmentExpression& expression,
                                     bool* ok) {
  if (ok != nullptr) { *ok = false; }
  if (expression.case_ge_thresholds_integral) {
    long long source = 0;
    if (ParseLongLongValue(source_value, &source)) {
      for (const auto& [threshold, value] :
           expression.case_ge_integer_thresholds) {
        if (source >= threshold) {
          if (ok != nullptr) { *ok = true; }
          return value;
        }
      }
      if (expression.case_fallback.has_value()) {
        if (ok != nullptr) { *ok = true; }
        return *expression.case_fallback;
      }
      return {};
    }
  }
  long double source = 0.0;
  if (!ParseLongDoubleValue(source_value, &source)) { return {}; }
  for (const auto& [threshold, value] : expression.case_ge_thresholds) {
    if (source >= threshold) {
      if (ok != nullptr) { *ok = true; }
      return value;
    }
  }
  if (expression.case_fallback.has_value()) {
    if (ok != nullptr) { *ok = true; }
    return *expression.case_fallback;
  }
  return {};
}

bool ParseLongDoubleValue(const std::string& value, long double* out) {
  if (out == nullptr || value.empty() || value == "<NULL>") { return false; }
  try {
    std::size_t consumed = 0;
    const long double parsed = std::stold(value, &consumed);
    if (consumed != value.size()) { return false; }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseLongLongValue(const std::string& value, long long* out) {
  if (out == nullptr || value.empty() || value == "<NULL>") { return false; }
  errno = 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(value.c_str(), &end, 10);
  if (errno == ERANGE || end == value.c_str() ||
      end != value.c_str() + value.size()) {
    return false;
  }
  *out = parsed;
  return true;
}

bool LooksIntegralText(const std::string& value) {
  if (value.empty()) { return false; }
  std::size_t index = value.front() == '-' ? 1 : 0;
  if (index >= value.size()) { return false; }
  for (; index < value.size(); ++index) {
    if (value[index] < '0' || value[index] > '9') { return false; }
  }
  return true;
}

std::string FormatArithmeticResult(long double value, bool integral) {
  if (integral) {
    return std::to_string(static_cast<long long>(value));
  }
  std::ostringstream out;
  out << std::setprecision(18) << value;
  return out.str();
}

EngineApiDiagnostic ApplyUpdateAssignmentExpressions(
    std::vector<UpdateAssignmentExpression>* expressions,
    std::vector<std::pair<std::string, std::string>>* values) {
  if (values == nullptr) {
    return MakeInvalidRequestDiagnostic("dml.update_rows", "assignment_values_required");
  }
  if (expressions == nullptr) {
    return MakeInvalidRequestDiagnostic("dml.update_rows", "assignment_plan_required");
  }
  for (auto& expression : *expressions) {
    std::string new_value = expression.literal_value;
    const std::string* source_value =
        expression.source_column.empty()
            ? nullptr
            : CachedCrudFieldValuePtr(*values,
                                      expression.source_column,
                                      &expression.cached_source_index);
    if (expression.operation == "copy_column") {
      new_value = source_value == nullptr ? std::string{} : *source_value;
    } else if (expression.operation == "concat") {
      new_value = (source_value == nullptr ? std::string{} : *source_value) +
                  expression.literal_value;
    } else if (expression.operation == "case_ge_thresholds") {
      bool case_ok = false;
      new_value = EvaluateCaseGeThresholds(
          source_value == nullptr ? std::string{} : *source_value,
          expression,
          &case_ok);
      if (!case_ok) {
        return MakeInvalidRequestDiagnostic("dml.update_rows",
                                            "assignment_case_threshold_evaluation_failed");
      }
    } else if (expression.operation == "add" ||
               expression.operation == "subtract" ||
               expression.operation == "multiply") {
      long double left = 0.0;
      long double right = 0.0;
      if (source_value == nullptr ||
          !ParseLongDoubleValue(*source_value, &left) ||
          !ParseLongDoubleValue(expression.literal_value, &right)) {
        return MakeInvalidRequestDiagnostic("dml.update_rows", "assignment_arithmetic_requires_numeric_values");
      }
      long double computed = left + right;
      if (expression.operation == "subtract") {
        computed = left - right;
      } else if (expression.operation == "multiply") {
        computed = left * right;
      }
      new_value = FormatArithmeticResult(computed,
                                         LooksIntegralText(*source_value) &&
                                             LooksIntegralText(expression.literal_value));
    }
    std::string* target_value =
        CachedMutableCrudFieldValuePtr(values,
                                       expression.target_column,
                                       &expression.cached_target_index);
    if (target_value != nullptr) {
      *target_value = std::move(new_value);
    } else {
      values->push_back({expression.target_column, std::move(new_value)});
      expression.cached_target_index = values->size() - 1;
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool DeleteOptionEnabled(const EngineDeleteRowsRequest& request,
                         std::string_view option) {
  return std::find(request.option_envelopes.begin(),
                   request.option_envelopes.end(),
                   option) != request.option_envelopes.end();
}

std::uint64_t DeleteOptionU64(const EngineDeleteRowsRequest& request,
                              std::string_view prefix,
                              std::uint64_t fallback) {
  for (const auto& option : request.option_envelopes) {
    if (!StartsWith(option, prefix)) {
      continue;
    }
    try {
      return static_cast<std::uint64_t>(std::stoull(option.substr(prefix.size())));
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

std::string DeleteOptionValue(const EngineDeleteRowsRequest& request,
                              std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

void NormalizeDeletePredicateFromLoweredOptions(EngineDeleteRowsRequest* request) {
  if (request == nullptr) { return; }
  const std::string predicate_kind = DeleteOptionValue(*request, "predicate_kind:");
  const std::string predicate_column = DeleteOptionValue(*request, "predicate_column:");
  const std::string predicate_value = DeleteOptionValue(*request, "predicate_value:");
  const std::string predicate_value_type =
      DeleteOptionValue(*request, "predicate_value_type:");
  if (!predicate_kind.empty()) {
    request->delete_predicate.predicate_kind = predicate_kind;
  }
  if (!predicate_column.empty()) {
    request->delete_predicate.canonical_predicate_envelope = predicate_column;
  }
  if (request->delete_predicate.bound_values.empty() && !predicate_value.empty()) {
    EngineTypedValue typed;
    typed.descriptor.descriptor_kind = "scalar";
    typed.descriptor.canonical_type_name =
        predicate_value_type.empty() ? "text" : predicate_value_type;
    typed.descriptor.encoded_descriptor =
        "type=" + typed.descriptor.canonical_type_name;
    typed.is_null = predicate_value == "<NULL>";
    typed.encoded_value = typed.is_null ? std::string{} : predicate_value;
    if (typed.is_null) {
      typed.setState(EngineValueState::sql_null);
    }
    request->delete_predicate.bound_values.push_back(std::move(typed));
  }
}

std::string DeleteSurfaceVariant(const EngineDeleteRowsRequest& request) {
  std::string variant = request.delete_surface_variant;
  if (variant.empty()) {
    variant = DeleteOptionValue(request, "dml_surface_variant:");
  }
  if (variant.empty()) {
    variant = DeleteOptionValue(request, "delete_surface_variant:");
  }
  if (variant.empty()) {
    variant = "delete";
  }
  for (char& c : variant) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return variant;
}

EngineApiU64 DeleteBatchLimitRows(const EngineDeleteRowsRequest& request) {
  if (request.batch_limit_rows != 0) {
    return request.batch_limit_rows;
  }
  return DeleteOptionU64(request, "batch_limit:", 0);
}

bool IsUpdateEqualityPredicate(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "column_equals" &&
         !predicate.canonical_predicate_envelope.empty() &&
         !predicate.bound_values.empty();
}

bool IsUpdateRangePredicate(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "column_range" &&
         !predicate.canonical_predicate_envelope.empty();
}

bool IsUpdateRowScanPredicate(const EnginePredicateEnvelope& predicate) {
  return predicate.predicate_kind == "always_false" ||
         predicate.predicate_kind == "columns_all_null" ||
         predicate.predicate_kind == "columns_all_not_null" ||
         predicate.predicate_kind == "column_equals_column_or_left_null" ||
         predicate.predicate_kind == "column_mod_equals" ||
         predicate.predicate_kind == "column_in_list" ||
         predicate.predicate_kind == "column_less_or_null" ||
         predicate.predicate_kind == "column_less" ||
         predicate.predicate_kind == "column_less_equal" ||
         predicate.predicate_kind == "column_greater" ||
         predicate.predicate_kind == "column_greater_equal" ||
         predicate.predicate_kind == "column_not_equals";
}

std::string LowerAsciiCopy(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::uint64_t UpdateMaxCommittedCrudTransactionId(const MgaRelationReadView& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, status] : state.transactions) {
    if (status == "committed" || status == "archived") {
      max_committed = std::max(max_committed, tx);
    }
  }
  return max_committed;
}

std::uint64_t UpdateVisibilityHighWaterForContext(
    const MgaRelationReadView& state,
    const EngineRequestContext& context) {
  const std::string isolation = context.transaction_isolation_level.empty()
                                    ? std::string("read_committed")
                                    : LowerAsciiCopy(context.transaction_isolation_level);
  if ((isolation == "snapshot" || isolation == "repeatable_read" ||
       isolation == "serializable") &&
      context.snapshot_visible_through_local_transaction_id != 0) {
    return context.snapshot_visible_through_local_transaction_id;
  }
  return UpdateMaxCommittedCrudTransactionId(state);
}

bool UpdateRowVersionVisibleWithHighWater(
    const MgaRelationReadView& state,
    const CrudRowVersionRecord& row,
    const EngineRequestContext& context,
    std::uint64_t visible_through) {
  if (!row.temporary_session_uuid.empty() &&
      row.temporary_session_uuid != context.session_uuid.canonical) {
    return false;
  }
  const std::uint64_t observer_tx = context.local_transaction_id;
  if (row.creator_tx == observer_tx) {
    const auto own = state.transactions.find(row.creator_tx);
    if (own != state.transactions.end() &&
        (own->second == "active" || own->second == "preparing" ||
         own->second == "prepared")) {
      return true;
    }
  }
  const auto it = state.transactions.find(row.creator_tx);
  if (it == state.transactions.end()) { return false; }
  if (it->second != "committed" && it->second != "archived") { return false; }
  return visible_through == 0 || row.creator_tx <= visible_through;
}

bool AppendOnlyUpdateCandidateRefs(
    const MgaRelationReadView& state,
    const std::string& table_uuid,
    const EngineRequestContext& context,
    std::vector<const CrudRowVersionRecord*>* rows) {
  if (rows == nullptr) { return false; }
  rows->clear();
  std::unordered_set<std::string> seen_row_uuids;
  std::size_t target_row_count = 0;
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid) { continue; }
    ++target_row_count;
    if (row.deleted || !row.previous_version_uuid.empty() ||
        row.previous_sequence != 0) {
      rows->clear();
      return false;
    }
    if (!seen_row_uuids.insert(row.row_uuid).second) {
      rows->clear();
      return false;
    }
  }
  rows->reserve(target_row_count);
  const std::uint64_t visible_through =
      UpdateVisibilityHighWaterForContext(state, context);
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid) { continue; }
    if (UpdateRowVersionVisibleWithHighWater(state, row, context, visible_through)) {
      rows->push_back(&row);
    }
  }
  return true;
}

std::vector<std::string> RowUuidListFromPredicate(
    const EnginePredicateEnvelope& predicate) {
  std::vector<std::string> row_uuids;
  if (predicate.predicate_kind != "row_uuid_in_list") {
    return row_uuids;
  }
  row_uuids.reserve(predicate.bound_values.size());
  for (const auto& bound : predicate.bound_values) {
    if (!bound.encoded_value.empty()) {
      row_uuids.push_back(bound.encoded_value);
    }
  }
  return row_uuids;
}

std::string PredicateDigest(const EnginePredicateEnvelope& predicate) {
  std::string digest = predicate.predicate_kind + ":" +
                       predicate.canonical_predicate_envelope + ":" +
                       std::to_string(predicate.bound_values.size());
  for (const auto& value : predicate.bound_values) {
    digest += ":" + value.encoded_value;
  }
  return digest;
}

EngineTypedValue TextPredicateBoundValue(std::string value, std::string type_name = "text") {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = type_name.empty() ? "text" : std::move(type_name);
  typed.descriptor.encoded_descriptor = "type=" + typed.descriptor.canonical_type_name;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

const std::string* CrudFieldValuePtr(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& field) {
  for (const auto& [name, value] : values) {
    if (name == field) { return &value; }
  }
  return nullptr;
}

std::size_t FindCrudFieldValueIndex(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& field) {
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (values[index].first == field) { return index; }
  }
  const std::string lowered_field = LowerAsciiCopy(field);
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (LowerAsciiCopy(values[index].first) == lowered_field) {
      return index;
    }
  }
  return std::string::npos;
}

const std::string* CachedCrudFieldValuePtr(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& field,
    std::size_t* cached_index) {
  if (cached_index != nullptr && *cached_index < values.size() &&
      values[*cached_index].first == field) {
    return &values[*cached_index].second;
  }
  const std::size_t found = FindCrudFieldValueIndex(values, field);
  if (cached_index != nullptr) { *cached_index = found; }
  return found == std::string::npos ? nullptr : &values[found].second;
}

std::string* CachedMutableCrudFieldValuePtr(
    std::vector<std::pair<std::string, std::string>>* values,
    const std::string& field,
    std::size_t* cached_index) {
  if (values == nullptr) { return nullptr; }
  if (cached_index != nullptr && *cached_index < values->size() &&
      (*values)[*cached_index].first == field) {
    return &(*values)[*cached_index].second;
  }
  const std::size_t found = FindCrudFieldValueIndex(*values, field);
  if (cached_index != nullptr) { *cached_index = found; }
  return found == std::string::npos ? nullptr : &(*values)[found].second;
}

bool TryParseFiniteDoubleNoThrow(const std::string& value, double* out) {
  if (out == nullptr || value.empty() || value == "<NULL>") {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (errno == ERANGE || end == value.c_str() ||
      end != value.c_str() + value.size() || !std::isfinite(parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

struct PreparedUpdatePredicate {
  bool column_less_or_null_numeric = false;
  bool column_compare_numeric = false;
  bool column_in_list_hash = false;
  std::string column_name;
  double numeric_bound = 0.0;
  std::string compare_kind;
  std::unordered_set<std::string> in_list_values;
  std::size_t cached_column_index = std::string::npos;
};

PreparedUpdatePredicate PrepareUpdatePredicate(
    const EnginePredicateEnvelope& predicate) {
  PreparedUpdatePredicate prepared;
  if (predicate.predicate_kind == "column_less_or_null" &&
      !predicate.canonical_predicate_envelope.empty() &&
      !predicate.bound_values.empty() &&
      TryParseFiniteDoubleNoThrow(predicate.bound_values.front().encoded_value,
                                  &prepared.numeric_bound)) {
    prepared.column_less_or_null_numeric = true;
    prepared.column_name = predicate.canonical_predicate_envelope;
  }
  if ((predicate.predicate_kind == "column_less" ||
       predicate.predicate_kind == "column_less_equal" ||
       predicate.predicate_kind == "column_greater" ||
       predicate.predicate_kind == "column_greater_equal" ||
       predicate.predicate_kind == "column_not_equals") &&
      !predicate.canonical_predicate_envelope.empty() &&
      !predicate.bound_values.empty() &&
      TryParseFiniteDoubleNoThrow(predicate.bound_values.front().encoded_value,
                                  &prepared.numeric_bound)) {
    prepared.column_compare_numeric = true;
    prepared.compare_kind = predicate.predicate_kind;
    prepared.column_name = predicate.canonical_predicate_envelope;
  }
  if (predicate.predicate_kind == "column_in_list" &&
      !predicate.canonical_predicate_envelope.empty() &&
      !predicate.bound_values.empty()) {
    prepared.column_in_list_hash = true;
    prepared.column_name = predicate.canonical_predicate_envelope;
    prepared.in_list_values.reserve(predicate.bound_values.size());
    for (const auto& bound : predicate.bound_values) {
      prepared.in_list_values.insert(bound.encoded_value);
    }
  }
  return prepared;
}

bool CrudRowMatchesPreparedUpdatePredicate(
    const CrudRowVersionRecord& row,
    const EnginePredicateEnvelope& predicate,
    PreparedUpdatePredicate* prepared) {
  if (prepared == nullptr) {
    return CrudRowMatchesPredicate(row, predicate);
  }
  if (prepared->column_less_or_null_numeric) {
    const auto* value = CachedCrudFieldValuePtr(row.values,
                                                prepared->column_name,
                                                &prepared->cached_column_index);
    if (value == nullptr || value->empty() || *value == "<NULL>") {
      return true;
    }
    double parsed = 0.0;
    if (!TryParseFiniteDoubleNoThrow(*value, &parsed)) {
      return CrudRowMatchesPredicate(row, predicate);
    }
    return parsed < prepared->numeric_bound;
  }
  if (prepared->column_compare_numeric) {
    const auto* value = CachedCrudFieldValuePtr(row.values,
                                                prepared->column_name,
                                                &prepared->cached_column_index);
    double parsed = 0.0;
    if (value == nullptr || !TryParseFiniteDoubleNoThrow(*value, &parsed)) {
      return CrudRowMatchesPredicate(row, predicate);
    }
    if (prepared->compare_kind == "column_less") {
      return parsed < prepared->numeric_bound;
    }
    if (prepared->compare_kind == "column_less_equal") {
      return parsed <= prepared->numeric_bound;
    }
    if (prepared->compare_kind == "column_greater") {
      return parsed > prepared->numeric_bound;
    }
    if (prepared->compare_kind == "column_greater_equal") {
      return parsed >= prepared->numeric_bound;
    }
    return parsed != prepared->numeric_bound;
  }
  if (prepared->column_in_list_hash) {
    const auto* value = CachedCrudFieldValuePtr(row.values,
                                                prepared->column_name,
                                                &prepared->cached_column_index);
    return prepared->in_list_values.find(value == nullptr ? std::string{} : *value) !=
           prepared->in_list_values.end();
  }
  return CrudRowMatchesPredicate(row, predicate);
}

bool VisibleCrudRowRefsForContext(
    const MgaRelationReadView& state,
    const std::string& table_uuid,
    const EngineRequestContext& context,
    std::vector<const CrudRowVersionRecord*>* rows) {
  if (rows == nullptr) { return false; }
  rows->clear();
  if (table_uuid.empty()) { return false; }
  std::unordered_map<std::string, const CrudRowVersionRecord*> newest_visible_by_uuid;
  newest_visible_by_uuid.reserve(state.row_versions.size());
  const std::uint64_t visible_through =
      UpdateVisibilityHighWaterForContext(state, context);
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid ||
        !UpdateRowVersionVisibleWithHighWater(state, row, context, visible_through)) {
      continue;
    }
    const auto found = newest_visible_by_uuid.find(row.row_uuid);
    if (found == newest_visible_by_uuid.end() ||
        row.sequence > found->second->sequence) {
      newest_visible_by_uuid[row.row_uuid] = &row;
    }
  }
  rows->reserve(newest_visible_by_uuid.size());
  for (const auto& [row_uuid, row] : newest_visible_by_uuid) {
    (void)row_uuid;
    if (row != nullptr && !row->deleted) {
      rows->push_back(row);
    }
  }
  return true;
}

struct DmlProjectionPredicateResolution {
  bool attempted{false};
  bool ok{true};
  EnginePredicateEnvelope predicate;
  EngineApiDiagnostic diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  std::vector<EngineEvidenceReference> evidence;
};

DmlProjectionPredicateResolution ResolveColumnInProjectionPredicate(
    const EngineUpdateRowsRequest& request,
    const MgaRelationReadView& state) {
  DmlProjectionPredicateResolution resolution;
  if (request.update_predicate.predicate_kind != "column_in_projection") {
    return resolution;
  }
  resolution.attempted = true;
  resolution.ok = false;
  const std::string source_uuid = UpdateOptionText(request, "source_uuid:");
  const std::string select_column = UpdateOptionText(request, "subquery_select_column:");
  const std::string subquery_predicate_kind =
      UpdateOptionText(request, "subquery_predicate_kind:");
  const std::string subquery_predicate_column =
      UpdateOptionText(request, "subquery_predicate_column:");
  const std::string subquery_predicate_value =
      UpdateOptionText(request, "subquery_predicate_value:");
  const std::string subquery_predicate_value_type =
      UpdateOptionText(request, "subquery_predicate_value_type:");
  if (source_uuid.empty() || select_column.empty() ||
      subquery_predicate_kind.empty() || subquery_predicate_column.empty()) {
    resolution.diagnostic =
        MakeInvalidRequestDiagnostic("dml.update_rows",
                                     "subquery_predicate_descriptor_incomplete");
    return resolution;
  }
  const auto source_table = FindVisibleMgaTable(state,
                                                 source_uuid,
                                                 request.context.local_transaction_id);
  if (!source_table) {
    resolution.diagnostic =
        MakeInvalidRequestDiagnostic("dml.update_rows",
                                     "subquery_source_table_not_visible");
    return resolution;
  }

  EnginePredicateEnvelope source_predicate;
  source_predicate.predicate_kind = subquery_predicate_kind;
  source_predicate.canonical_predicate_envelope = subquery_predicate_column;
  if (!subquery_predicate_value.empty()) {
    source_predicate.bound_values.push_back(
        TextPredicateBoundValue(subquery_predicate_value,
                                subquery_predicate_value_type));
  }

  EnginePredicateEnvelope resolved;
  resolved.predicate_kind = "column_in_list";
  resolved.canonical_predicate_envelope =
      request.update_predicate.canonical_predicate_envelope;
  std::set<std::string> admitted_values;
  std::vector<const CrudRowVersionRecord*> source_row_refs;
  auto prepared_source_predicate = PrepareUpdatePredicate(source_predicate);
  std::size_t select_column_index = std::string::npos;
  if (AppendOnlyUpdateCandidateRefs(state,
                                    source_uuid,
                                    request.context,
                                    &source_row_refs)) {
    for (const auto* row : source_row_refs) {
      if (row == nullptr ||
          !CrudRowMatchesPreparedUpdatePredicate(*row,
                                                 source_predicate,
                                                 &prepared_source_predicate)) {
        continue;
      }
      const auto* selected_value = CachedCrudFieldValuePtr(row->values,
                                                           select_column,
                                                           &select_column_index);
      const std::string value =
          selected_value == nullptr ? std::string{} : *selected_value;
      if (!value.empty() && value != "<NULL>") {
        admitted_values.insert(value);
      }
    }
    resolution.evidence.push_back({"dml_subquery_source_rows",
                                   "append_only_ref_fast_path"});
  } else {
    const auto source_rows = VisibleMgaRowsForContext(state,
                                                       source_uuid,
                                                       request.context);
    for (const auto& row : source_rows) {
      if (!CrudRowMatchesPreparedUpdatePredicate(row,
                                                 source_predicate,
                                                 &prepared_source_predicate)) {
        continue;
      }
      const auto* selected_value = CachedCrudFieldValuePtr(row.values,
                                                           select_column,
                                                           &select_column_index);
      const std::string value =
          selected_value == nullptr ? std::string{} : *selected_value;
      if (!value.empty() && value != "<NULL>") {
        admitted_values.insert(value);
      }
    }
  }
  for (const auto& value : admitted_values) {
    resolved.bound_values.push_back(TextPredicateBoundValue(value));
  }
  resolution.ok = true;
  resolution.predicate = std::move(resolved);
  resolution.evidence.push_back({"dml_subquery_predicate_materialized",
                                 "column_in_projection_to_column_in_list"});
  resolution.evidence.push_back({"dml_subquery_materialized_value_count",
                                 std::to_string(resolution.predicate.bound_values.size())});
  return resolution;
}

std::string CrudIndexResolvedFamily(const CrudIndexRecord& index) {
  return index.family.empty() ? CrudIndexFamilyForProfile(index.profile) : index.family;
}

bool IndexUsableForUpdateCandidateStream(const CrudIndexRecord& index,
                                         const EnginePredicateEnvelope& predicate) {
  if (!CrudIndexSupportsPredicate(index, predicate)) {
    return false;
  }
  const auto family = CrudIndexResolvedFamily(index);
  if (IsUpdateEqualityPredicate(predicate)) {
    return family == kCrudIndexFamilyBtree || family == kCrudIndexFamilyHash ||
           family.empty();
  }
  if (IsUpdateRangePredicate(predicate)) {
    return family == kCrudIndexFamilyBtree || family.empty();
  }
  return false;
}

std::optional<CrudIndexRecord> SelectUpdateCandidateStreamIndex(
    const std::vector<CrudIndexRecord>& visible_indexes,
    const EnginePredicateEnvelope& predicate,
    bool* unusable_index_present) {
  if (unusable_index_present != nullptr) {
    *unusable_index_present = false;
  }
  for (const auto& index : visible_indexes) {
    if (!CrudIndexSupportsPredicate(index, predicate)) {
      continue;
    }
    if (IndexUsableForUpdateCandidateStream(index, predicate)) {
      return index;
    }
    if (unusable_index_present != nullptr) {
      *unusable_index_present = true;
    }
  }
  return std::nullopt;
}

bool UpdateCandidateStreamNeedsIndexEntries(
    const EngineUpdateRowsRequest& request,
    const std::vector<CrudIndexRecord>& visible_indexes) {
  if (!IsUpdateEqualityPredicate(request.update_predicate) &&
      !IsUpdateRangePredicate(request.update_predicate)) {
    return false;
  }
  bool unusable_index_present = false;
  return SelectUpdateCandidateStreamIndex(visible_indexes,
                                          request.update_predicate,
                                          &unusable_index_present).has_value();
}

std::optional<CrudRowVersionRecord> FindVisibleRowUuidCandidate(
    const MgaRelationReadView& state,
    const std::string& table_uuid,
    const std::string& row_uuid,
    const EngineRequestContext& context) {
  std::vector<CrudRowVersionRecord> versions;
  for (const auto& row : state.row_versions) {
    if (row.table_uuid == table_uuid && row.row_uuid == row_uuid) {
      versions.push_back(row);
    }
  }
  std::sort(versions.begin(), versions.end(), [](const auto& left, const auto& right) {
    return left.sequence > right.sequence;
  });
  for (const auto& row : versions) {
    if (!MgaRowVersionVisibleToContext(state, row, context)) {
      continue;
    }
    if (!row.deleted) {
      return row;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::vector<CrudRowVersionRecord> FindVisibleRowUuidCandidates(
    const MgaRelationReadView& state,
    const std::string& table_uuid,
    const std::vector<std::string>& row_uuids,
    const EngineRequestContext& context) {
  std::vector<CrudRowVersionRecord> rows;
  if (row_uuids.empty()) {
    return rows;
  }

  std::unordered_set<std::string> requested;
  requested.reserve(row_uuids.size());
  for (const auto& row_uuid : row_uuids) {
    if (!row_uuid.empty()) {
      requested.insert(row_uuid);
    }
  }
  if (requested.empty()) {
    return rows;
  }

  std::unordered_map<std::string, CrudRowVersionRecord> newest_visible_by_uuid;
  newest_visible_by_uuid.reserve(requested.size());
  for (const auto& row : state.row_versions) {
    if (row.table_uuid != table_uuid ||
        requested.find(row.row_uuid) == requested.end() ||
        !MgaRowVersionVisibleToContext(state, row, context)) {
      continue;
    }
    const auto found = newest_visible_by_uuid.find(row.row_uuid);
    if (found == newest_visible_by_uuid.end() ||
        row.sequence > found->second.sequence) {
      newest_visible_by_uuid[row.row_uuid] = row;
    }
  }

  rows.reserve(row_uuids.size());
  for (const auto& row_uuid : row_uuids) {
    const auto found = newest_visible_by_uuid.find(row_uuid);
    if (found != newest_visible_by_uuid.end() && !found->second.deleted) {
      rows.push_back(found->second);
    }
  }
  return rows;
}

DmlTargetAccessPlanRequest BuildUpdateTargetAccessPlanRequest(
    const EngineUpdateRowsRequest& request,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& visible_indexes,
    bool* unsupported_predicate,
    bool* unusable_index_present) {
  if (unsupported_predicate != nullptr) {
    *unsupported_predicate = false;
  }
  if (unusable_index_present != nullptr) {
    *unusable_index_present = false;
  }

  DmlTargetAccessPlanRequest plan_request;
  plan_request.mutation_kind = "dml.update_rows";
  plan_request.database_uuid = request.context.database_uuid.canonical;
  plan_request.relation_uuid = table.table_uuid;
  plan_request.relation_present = true;
  plan_request.predicate_kind = request.update_predicate.predicate_kind;
  plan_request.predicate_descriptor_digest = PredicateDigest(request.update_predicate);
  plan_request.access_descriptor_present = true;
  plan_request.security_policy_digest =
      request.context.principal_uuid.canonical + ":" +
      request.context.current_role_uuid.canonical + ":" +
      std::to_string(request.context.security_epoch);
  plan_request.redaction_policy_digest =
      "resource_epoch:" + std::to_string(request.context.resource_epoch);
  plan_request.access_policy_digest =
      request.context.session_uuid.canonical + ":" +
      std::to_string(request.context.resource_epoch);
  plan_request.collation_profile_digest =
      request.context.identifier_profile_uuid + ":" +
      request.context.language_context.language_tag;
  plan_request.local_transaction_id = request.context.local_transaction_id;
  plan_request.mga_visibility_recheck_planned =
      !UpdateOptionEnabled(request, "odf031.disable_mga_visibility_recheck=true");
  plan_request.security_recheck_planned =
      !UpdateOptionEnabled(request, "odf031.disable_security_recheck=true");
  const bool force_missing_security_context =
      UpdateOptionEnabled(request, "odf031.force_missing_security_context=true");
  plan_request.grants_proven =
      request.context.security_context_present && !force_missing_security_context;
  plan_request.security_context_present =
      request.context.security_context_present && !force_missing_security_context;
  plan_request.parser_or_reference_authority =
      request.update_predicate.predicate_kind == "reference_bulk" ||
      UpdateOptionEnabled(request, "odf031.parser_or_reference_authority=true");
  const std::uint64_t observed_catalog_epoch =
      request.bound_object_identity.catalog_generation_id != 0
          ? request.bound_object_identity.catalog_generation_id
          : request.context.catalog_generation_id;
  const std::uint64_t observed_security_epoch =
      request.bound_object_identity.security_epoch != 0
          ? request.bound_object_identity.security_epoch
          : request.context.security_epoch;
  const std::uint64_t observed_policy_epoch =
      request.bound_object_identity.resource_epoch != 0
          ? request.bound_object_identity.resource_epoch
          : request.context.resource_epoch;
  plan_request.observed_catalog_epoch = observed_catalog_epoch;
  plan_request.current_catalog_epoch = request.context.catalog_generation_id;
  plan_request.observed_security_epoch = observed_security_epoch;
  plan_request.current_security_epoch = request.context.security_epoch;
  plan_request.observed_policy_epoch = observed_policy_epoch;
  plan_request.current_policy_epoch = request.context.resource_epoch;
  plan_request.index_epoch = observed_catalog_epoch;
  plan_request.object_epoch = observed_catalog_epoch;
  plan_request.compatibility_epoch =
      request.context.snapshot_visible_through_local_transaction_id != 0
          ? request.context.snapshot_visible_through_local_transaction_id
          : request.context.local_transaction_id;
  plan_request.observed_stats_epoch =
      UpdateOptionU64(request, "odf031.observed_stats_epoch=", 0);
  plan_request.current_stats_epoch =
      UpdateOptionU64(request, "odf031.current_stats_epoch=", 0);

  if (UpdateMutationWindowActive(request)) {
    plan_request.explicit_table_scan_fallback = true;
    if (request.update_predicate.predicate_kind.empty()) {
      plan_request.predicate_kind = "all_visible_rows";
    }
    return plan_request;
  }

  if (request.update_predicate.predicate_kind.empty()) {
    plan_request.explicit_table_scan_fallback = true;
    plan_request.predicate_kind = "all_visible_rows";
    return plan_request;
  }
  if (request.update_predicate.predicate_kind == "row_uuid_match" &&
      !request.update_predicate.canonical_predicate_envelope.empty()) {
    plan_request.predicate_kind = "row_uuid_match";
    plan_request.row_uuid = request.update_predicate.canonical_predicate_envelope;
    plan_request.estimated_rows = 1;
    return plan_request;
  }
  auto row_uuid_list = RowUuidListFromPredicate(request.update_predicate);
  if (!row_uuid_list.empty()) {
    plan_request.predicate_kind = "row_uuid_in_list";
    plan_request.row_uuids = std::move(row_uuid_list);
    plan_request.estimated_rows =
        static_cast<std::uint64_t>(plan_request.row_uuids.size());
    return plan_request;
  }
  if (IsUpdateEqualityPredicate(request.update_predicate) ||
      IsUpdateRangePredicate(request.update_predicate)) {
    const auto index = SelectUpdateCandidateStreamIndex(visible_indexes,
                                                        request.update_predicate,
                                                        unusable_index_present);
    if (index) {
      plan_request.predicate_kind =
          IsUpdateEqualityPredicate(request.update_predicate)
              ? (index->unique ? "unique_eq" : "scalar_eq")
              : "scalar_range";
      plan_request.index_uuid = index->index_uuid;
      plan_request.index_family = CrudIndexResolvedFamily(*index);
      plan_request.index_unique = index->unique;
      plan_request.estimated_rows = index->unique ? 1 : 0;
      return plan_request;
    }
    plan_request.explicit_table_scan_fallback = true;
    return plan_request;
  }
  if (IsUpdateRowScanPredicate(request.update_predicate)) {
    plan_request.explicit_table_scan_fallback = true;
    return plan_request;
  }

  if (unsupported_predicate != nullptr) {
    *unsupported_predicate = true;
  }
  plan_request.explicit_table_scan_fallback = true;
  return plan_request;
}

void AddTargetAccessPlanEvidence(const DmlTargetAccessPlan& plan,
                                 std::string_view target_access_kind_evidence,
                                 std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({"dml_target_access_plan",
                       SerializeDmlTargetAccessPlanEvidence(plan)});
  evidence->push_back({std::string(target_access_kind_evidence),
                       DmlTargetAccessKindName(plan.access_kind)});
  for (const auto& entry : plan.evidence) {
    evidence->push_back({"dml_target_access_plan_evidence", entry});
  }
  for (const auto& diagnostic : plan.diagnostics) {
    evidence->push_back({"dml_target_access_plan_refusal", diagnostic});
  }
}

void AddDmlHotPointAdmissionEvidence(const DmlTargetAccessPlanRequest& plan_request,
                                     const std::string& row_uuid,
                                     std::vector<EngineEvidenceReference>* evidence) {
  std::vector<std::string> cache_evidence;
  DmlTargetAccessPlanRequest locator_request = plan_request;
  if (locator_request.row_uuid.empty()) {
    locator_request.row_uuid = row_uuid;
    locator_request.predicate_kind = "row_uuid_match";
    locator_request.predicate_descriptor_digest = "row_uuid_match:" + row_uuid;
    locator_request.row_uuids.clear();
  }
  AdmitDmlHotPointLookupCacheSuccessfulRowLocator(locator_request,
                                                  row_uuid,
                                                  &cache_evidence);
  for (const auto& item : cache_evidence) {
    evidence->push_back({"dml_hot_point_lookup_cache", item});
  }
}

void AppendDmlRowLocatorStreamEvidence(
    std::string_view prefix,
    const DmlRowLocatorStreamResult& stream,
    std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({std::string(prefix) + "_row_locator_stream",
                       stream.ok ? DmlRowLocatorStreamSourceName(stream.source)
                                 : "refused"});
  evidence->push_back({std::string(prefix) + "_row_locator_stream_ok",
                       stream.ok ? "true" : "false"});
  evidence->push_back({std::string(prefix) + "_row_locator_count",
                       std::to_string(stream.locators.size())});
  for (const auto& item : stream.evidence) {
    evidence->push_back({std::string(prefix) + "_row_locator_stream_evidence",
                         item.evidence_kind + "=" + item.evidence_id});
  }
}

DmlRowLocatorStreamResult BuildRouteLocatorStream(
    DmlRowLocatorStreamConsumer consumer,
    const DmlTargetAccessPlan& plan,
    bool table_scan_fallback_allowed = false,
    bool applicable_physical_index_exists = false) {
  DmlRowLocatorStreamRequest request;
  request.consumer = consumer;
  request.access_plan = plan;
  request.access_plan_engine_authority_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.parser_or_reference_authority = false;
  request.index_or_cache_finality_authority = false;
  request.table_scan_fallback_allowed = table_scan_fallback_allowed;
  request.applicable_physical_index_exists = applicable_physical_index_exists;
  return BuildDmlRowLocatorStream(request);
}

DmlTargetAccessPlan BuildRowUuidLocatorPlanFromRows(
    const DmlTargetAccessPlanRequest& base_request,
    const std::vector<CrudRowVersionRecord>& rows) {
  if (rows.empty()) {
    DmlTargetAccessPlan plan;
    plan.ok = true;
    plan.access_kind = DmlTargetAccessKind::row_uuid_list;
    plan.physical_access_kind = "row_uuid_lookup";
    plan.executor_capability = "row_uuid_lookup";
    plan.relation_uuid = base_request.relation_uuid;
    plan.predicate_kind = "row_uuid_in_list";
    plan.predicate_descriptor_digest =
        "irc052_empty_persisted_index_locator_stream";
    plan.index_uuid = base_request.index_uuid;
    plan.estimated_rows = 0;
    plan.evidence.push_back("dml_target_access_kind=row_uuid_list");
    plan.evidence.push_back("physical_index_tree_available=false");
    plan.evidence.push_back("irc060_required_for_physical_scan=true");
    return plan;
  }

  DmlTargetAccessPlanRequest locator_request = base_request;
  locator_request.index_uuid.clear();
  locator_request.index_unique = false;
  locator_request.index_family = "btree";
  locator_request.predicate_kind = rows.size() == 1 ? "row_uuid_match"
                                                    : "row_uuid_in_list";
  locator_request.predicate_descriptor_digest =
      "irc052_persisted_index_row_uuid_locator_stream:" +
      std::to_string(rows.size());
  locator_request.row_uuid = rows.size() == 1 ? rows.front().row_uuid : "";
  locator_request.row_uuids.clear();
  for (const auto& row : rows) {
    locator_request.row_uuids.push_back(row.row_uuid);
  }
  locator_request.estimated_rows = static_cast<std::uint64_t>(rows.size());
  return BuildDmlTargetAccessPlan(locator_request);
}

void AddUpdateCandidateFallbackEvidence(std::string reason,
                                        UpdateTargetCandidateStream* stream) {
  stream->evidence.push_back({"update_row_candidate_stream", "table_scan"});
  stream->evidence.push_back({"update_target_access_fallback", std::move(reason)});
  stream->evidence.push_back({"physical_index_tree_available", "false"});
  stream->evidence.push_back({"irc060_required_for_physical_scan", "true"});
  stream->evidence.push_back({"update_row_locator_stream",
                              "table_scan_fallback_no_applicable_locator"});
}

bool HasTargetAccessDiagnostic(const DmlTargetAccessPlan& plan,
                               std::string_view diagnostic) {
  return std::find(plan.diagnostics.begin(),
                   plan.diagnostics.end(),
                   diagnostic) != plan.diagnostics.end();
}

bool UnsafeTargetAccessRefusal(const DmlTargetAccessPlan& plan) {
  return HasTargetAccessDiagnostic(plan, "missing MGA recheck") ||
         HasTargetAccessDiagnostic(plan, "missing security recheck") ||
         HasTargetAccessDiagnostic(plan, "missing grants/security context") ||
         HasTargetAccessDiagnostic(plan, "stale catalog epoch") ||
         HasTargetAccessDiagnostic(plan, "stale security epoch") ||
         HasTargetAccessDiagnostic(plan, "stale policy epoch") ||
         HasTargetAccessDiagnostic(plan, "stale stats epoch") ||
         HasTargetAccessDiagnostic(plan, "unsafe parser/reference authority");
}

bool IsIndexTargetAccess(DmlTargetAccessKind access_kind) {
  return access_kind == DmlTargetAccessKind::unique_index_lookup ||
         access_kind == DmlTargetAccessKind::nonunique_index_lookup ||
         access_kind == DmlTargetAccessKind::range_index_lookup;
}

std::vector<CrudRowVersionRecord> ScanVisibleRowsMatchingPredicate(
    const MgaRelationReadView& state,
    const std::string& table_uuid,
    const EngineRequestContext& context,
    const EnginePredicateEnvelope& predicate,
    std::uint64_t limit) {
  std::vector<CrudRowVersionRecord> rows;
  for (const auto& row : VisibleMgaRowsForContext(state, table_uuid, context)) {
    if (!CrudRowMatchesPredicate(row, predicate)) {
      continue;
    }
    rows.push_back(row);
    if (limit != 0 && rows.size() >= limit) {
      break;
    }
  }
  return rows;
}

void AddDmlSummaryFallbacksFromEvidence(
    const std::vector<EngineEvidenceReference>& evidence,
    std::string_view evidence_kind,
    EngineDmlSummaryCounters* counters) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == evidence_kind) {
      AddDmlSummaryFallbackReason(counters, item.evidence_id);
    }
  }
}

EngineApiDiagnostic UpdateTargetAccessRefusalDiagnostic(
    const DmlTargetAccessPlan& plan) {
  const std::string detail = plan.diagnostics.empty()
                                 ? "target_access_plan_refused"
                                 : "target_access_plan_refused:" +
                                       plan.diagnostics.front();
  return MakeInvalidRequestDiagnostic("dml.update_rows", detail);
}

UpdateTargetCandidateStream BuildUpdateTargetCandidateStream(
    const EngineUpdateRowsRequest& request,
    const MgaRelationReadView& state,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& visible_indexes) {
  UpdateTargetCandidateStream stream;
  bool unsupported_predicate = false;
  bool unusable_index_present = false;
  const auto plan_request = BuildUpdateTargetAccessPlanRequest(request,
                                                               table,
                                                               visible_indexes,
                                                               &unsupported_predicate,
                                                               &unusable_index_present);
  stream.plan = BuildDmlTargetAccessPlan(plan_request);
  AddTargetAccessPlanEvidence(stream.plan,
                              "update_target_access_kind",
                              &stream.evidence);

  if (!stream.plan.ok) {
    if (UnsafeTargetAccessRefusal(stream.plan)) {
      stream.fail_closed = true;
      stream.diagnostic = UpdateTargetAccessRefusalDiagnostic(stream.plan);
      stream.evidence.push_back({"update_row_candidate_stream", "refused"});
      stream.evidence.push_back({"update_target_access_refusal",
                                 "fail_closed_unsafe_route"});
      return stream;
    }
    AddUpdateCandidateFallbackEvidence("target_access_plan_refused", &stream);
    return stream;
  }

  switch (stream.plan.access_kind) {
    case DmlTargetAccessKind::row_uuid_singleton: {
      stream.evidence.push_back({"update_row_candidate_stream", "row_uuid_singleton"});
      const auto locator_stream =
          BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::update,
                                  stream.plan);
      AppendDmlRowLocatorStreamEvidence("update", locator_stream, &stream.evidence);
      if (!locator_stream.ok) {
        stream.fail_closed = true;
        stream.diagnostic = locator_stream.diagnostic;
        return stream;
      }
      const auto row = FindVisibleRowUuidCandidate(state,
                                                   table.table_uuid,
                                                   stream.plan.row_uuid,
                                                   request.context);
      if (row && CrudRowMatchesPredicate(*row, request.update_predicate)) {
        stream.rows.push_back(*row);
        AddDmlHotPointAdmissionEvidence(plan_request,
                                        row->row_uuid,
                                        &stream.evidence);
      }
      stream.rows_ready = true;
      return stream;
    }
    case DmlTargetAccessKind::row_uuid_list: {
      stream.evidence.push_back({"update_row_candidate_stream", "row_uuid_list"});
      stream.evidence.push_back({"update_row_uuid_list_size",
                                 std::to_string(stream.plan.row_uuids.size())});
      const auto locator_stream =
          BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::update,
                                  stream.plan);
      AppendDmlRowLocatorStreamEvidence("update", locator_stream, &stream.evidence);
      if (!locator_stream.ok) {
        stream.fail_closed = true;
        stream.diagnostic = locator_stream.diagnostic;
        return stream;
      }
      stream.rows.reserve(stream.plan.row_uuids.size());
      const auto candidate_rows =
          FindVisibleRowUuidCandidates(state,
                                       table.table_uuid,
                                       stream.plan.row_uuids,
                                       request.context);
      stream.evidence.push_back({"update_row_uuid_list_lookup",
                                 "single_pass_mga_visibility"});
      for (const auto& row : candidate_rows) {
        if (CrudRowMatchesPredicate(row, request.update_predicate)) {
          stream.rows.push_back(row);
          AddDmlHotPointAdmissionEvidence(plan_request,
                                          row.row_uuid,
                                          &stream.evidence);
        }
      }
      stream.rows_ready = true;
      return stream;
    }
    case DmlTargetAccessKind::unique_index_lookup:
    case DmlTargetAccessKind::nonunique_index_lookup:
    case DmlTargetAccessKind::range_index_lookup: {
      const auto indexed = IndexedMgaRowsForPredicateForContext(state,
                                                                table.table_uuid,
                                                                request.update_predicate,
                                                                request.context,
                                                                0);
      stream.evidence.insert(stream.evidence.end(),
                             indexed.evidence.begin(),
                             indexed.evidence.end());
      if (indexed.index_used) {
        stream.rows = indexed.rows;
        if (stream.rows.empty()) {
          stream.rows = ScanVisibleRowsMatchingPredicate(state,
                                                         table.table_uuid,
                                                         request.context,
                                                         request.update_predicate,
                                                         0);
          stream.evidence.push_back(
              {"update_index_empty_visible_scan_fallback",
               stream.rows.empty() ? "no_visible_match"
                                   : "matched_visible_rows"});
          stream.evidence.push_back(
              {"update_index_empty_visible_scan_fallback_rows",
               std::to_string(stream.rows.size())});
        }
        stream.rows_ready = true;
        stream.evidence.push_back({"update_row_candidate_stream", "indexed_predicate"});
        stream.evidence.push_back({"index_lookup", indexed.index_evidence_id});
        stream.evidence.push_back({"physical_index_tree_available", "false"});
        stream.evidence.push_back({"irc060_required_for_physical_scan", "true"});
        const auto locator_plan =
            BuildRowUuidLocatorPlanFromRows(plan_request, stream.rows);
        const auto locator_stream =
            BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::update,
                                    locator_plan);
        AppendDmlRowLocatorStreamEvidence("update", locator_stream, &stream.evidence);
        if (!locator_stream.ok) {
          stream.fail_closed = true;
          stream.diagnostic = locator_stream.diagnostic;
          return stream;
        }
        stream.evidence.push_back({"update_row_locator_stream",
                                   "consumed_row_uuid_after_index_probe"});
        for (const auto& row : stream.rows) {
          AddDmlHotPointAdmissionEvidence(plan_request,
                                          row.row_uuid,
                                          &stream.evidence);
        }
        return stream;
      }
      if (indexed.index_refused) {
        stream.fail_closed = true;
        stream.diagnostic =
            indexed.diagnostic.detail.empty()
                ? MakeInvalidRequestDiagnostic("dml.update_rows",
                                               "mga_indexed_lookup_refused")
                : indexed.diagnostic;
        stream.evidence.push_back({"update_row_candidate_stream", "refused"});
        stream.evidence.push_back({"update_target_access_index_refusal",
                                   indexed.diagnostic.detail.empty()
                                       ? indexed.diagnostic.message_key
                                       : indexed.diagnostic.detail});
        stream.evidence.push_back({"update_target_access_refusal",
                                   "fail_closed_index_locator_stream"});
        return stream;
      } else {
        stream.fail_closed = true;
        stream.diagnostic = MakeInvalidRequestDiagnostic(
            "dml.update_rows",
            "planned_index_lookup_not_used");
        stream.evidence.push_back({"update_row_candidate_stream", "refused"});
        stream.evidence.push_back({"update_target_access_refusal",
                                   "planned_index_lookup_not_used"});
        return stream;
      }
    }
    case DmlTargetAccessKind::table_scan:
      {
        const auto locator_stream =
            BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::update,
                                    stream.plan,
                                    true,
                                    false);
        AppendDmlRowLocatorStreamEvidence("update", locator_stream, &stream.evidence);
        if (!locator_stream.ok) {
          stream.fail_closed = true;
          stream.diagnostic = locator_stream.diagnostic;
          return stream;
        }
      }
      AddUpdateCandidateFallbackEvidence(
          unsupported_predicate ? "unsupported predicate"
                                : (unusable_index_present ? "unusable index"
                                                          : "unindexed predicate"),
          &stream);
      return stream;
    case DmlTargetAccessKind::summary_pruned:
    case DmlTargetAccessKind::refused:
      AddUpdateCandidateFallbackEvidence("unsupported predicate", &stream);
      return stream;
  }
  AddUpdateCandidateFallbackEvidence("unsupported predicate", &stream);
  return stream;
}

DmlTargetAccessPlanRequest BuildDeleteTargetAccessPlanRequest(
    const EngineDeleteRowsRequest& request,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& visible_indexes,
    bool* unsupported_predicate,
    bool* unusable_index_present) {
  if (unsupported_predicate != nullptr) {
    *unsupported_predicate = false;
  }
  if (unusable_index_present != nullptr) {
    *unusable_index_present = false;
  }

  DmlTargetAccessPlanRequest plan_request;
  plan_request.mutation_kind = "dml.delete_rows";
  plan_request.database_uuid = request.context.database_uuid.canonical;
  plan_request.relation_uuid = table.table_uuid;
  plan_request.relation_present = true;
  plan_request.predicate_kind = request.delete_predicate.predicate_kind;
  plan_request.predicate_descriptor_digest = PredicateDigest(request.delete_predicate);
  plan_request.access_descriptor_present = true;
  plan_request.security_policy_digest =
      request.context.principal_uuid.canonical + ":" +
      request.context.current_role_uuid.canonical + ":" +
      std::to_string(request.context.security_epoch);
  plan_request.redaction_policy_digest =
      "resource_epoch:" + std::to_string(request.context.resource_epoch);
  plan_request.access_policy_digest =
      request.context.session_uuid.canonical + ":" +
      std::to_string(request.context.resource_epoch);
  plan_request.collation_profile_digest =
      request.context.identifier_profile_uuid + ":" +
      request.context.language_context.language_tag;
  plan_request.local_transaction_id = request.context.local_transaction_id;
  plan_request.mga_visibility_recheck_planned =
      !DeleteOptionEnabled(request, "odf032.disable_mga_visibility_recheck=true");
  plan_request.security_recheck_planned =
      !DeleteOptionEnabled(request, "odf032.disable_security_recheck=true");
  const bool force_missing_security_context =
      DeleteOptionEnabled(request, "odf032.force_missing_security_context=true");
  plan_request.grants_proven =
      request.context.security_context_present && !force_missing_security_context;
  plan_request.security_context_present =
      request.context.security_context_present && !force_missing_security_context;
  plan_request.parser_or_reference_authority =
      request.delete_predicate.predicate_kind == "reference_bulk" ||
      DeleteOptionEnabled(request, "odf032.parser_or_reference_authority=true");
  const std::uint64_t observed_catalog_epoch =
      request.bound_object_identity.catalog_generation_id != 0
          ? request.bound_object_identity.catalog_generation_id
          : request.context.catalog_generation_id;
  const std::uint64_t observed_security_epoch =
      request.bound_object_identity.security_epoch != 0
          ? request.bound_object_identity.security_epoch
          : request.context.security_epoch;
  const std::uint64_t observed_policy_epoch =
      request.bound_object_identity.resource_epoch != 0
          ? request.bound_object_identity.resource_epoch
          : request.context.resource_epoch;
  plan_request.observed_catalog_epoch = observed_catalog_epoch;
  plan_request.current_catalog_epoch = request.context.catalog_generation_id;
  plan_request.observed_security_epoch = observed_security_epoch;
  plan_request.current_security_epoch = request.context.security_epoch;
  plan_request.observed_policy_epoch = observed_policy_epoch;
  plan_request.current_policy_epoch = request.context.resource_epoch;
  plan_request.index_epoch = observed_catalog_epoch;
  plan_request.object_epoch = observed_catalog_epoch;
  plan_request.compatibility_epoch =
      request.context.snapshot_visible_through_local_transaction_id != 0
          ? request.context.snapshot_visible_through_local_transaction_id
          : request.context.local_transaction_id;
  plan_request.observed_stats_epoch =
      DeleteOptionU64(request, "odf032.observed_stats_epoch=", 0);
  plan_request.current_stats_epoch =
      DeleteOptionU64(request, "odf032.current_stats_epoch=", 0);

  if (DeleteMutationWindowActive(request)) {
    plan_request.explicit_table_scan_fallback = true;
    if (request.delete_predicate.predicate_kind.empty()) {
      plan_request.predicate_kind = "all_visible_rows";
    }
    return plan_request;
  }

  if (request.delete_predicate.predicate_kind.empty()) {
    plan_request.explicit_table_scan_fallback = true;
    plan_request.predicate_kind = "all_visible_rows";
    return plan_request;
  }
  if (request.delete_predicate.predicate_kind == "row_uuid_match" &&
      !request.delete_predicate.canonical_predicate_envelope.empty()) {
    plan_request.predicate_kind = "row_uuid_match";
    plan_request.row_uuid = request.delete_predicate.canonical_predicate_envelope;
    plan_request.estimated_rows = 1;
    return plan_request;
  }
  auto row_uuid_list = RowUuidListFromPredicate(request.delete_predicate);
  if (!row_uuid_list.empty()) {
    plan_request.predicate_kind = "row_uuid_in_list";
    plan_request.row_uuids = std::move(row_uuid_list);
    plan_request.estimated_rows =
        static_cast<std::uint64_t>(plan_request.row_uuids.size());
    return plan_request;
  }
  if (IsUpdateEqualityPredicate(request.delete_predicate) ||
      IsUpdateRangePredicate(request.delete_predicate)) {
    const auto index = SelectUpdateCandidateStreamIndex(visible_indexes,
                                                        request.delete_predicate,
                                                        unusable_index_present);
    if (index) {
      plan_request.predicate_kind =
          IsUpdateEqualityPredicate(request.delete_predicate)
              ? (index->unique ? "unique_eq" : "scalar_eq")
              : "scalar_range";
      plan_request.index_uuid = index->index_uuid;
      plan_request.index_family = CrudIndexResolvedFamily(*index);
      plan_request.index_unique = index->unique;
      plan_request.estimated_rows = index->unique ? 1 : 0;
      return plan_request;
    }
    plan_request.explicit_table_scan_fallback = true;
    return plan_request;
  }
  if (IsUpdateRowScanPredicate(request.delete_predicate)) {
    plan_request.explicit_table_scan_fallback = true;
    return plan_request;
  }

  if (unsupported_predicate != nullptr) {
    *unsupported_predicate = true;
  }
  plan_request.explicit_table_scan_fallback = true;
  return plan_request;
}

void AddDeleteCandidateFallbackEvidence(std::string reason,
                                        DeleteTargetCandidateStream* stream) {
  stream->evidence.push_back({"delete_row_candidate_stream", "table_scan"});
  stream->evidence.push_back({"delete_target_access_fallback", std::move(reason)});
  stream->evidence.push_back({"physical_index_tree_available", "false"});
  stream->evidence.push_back({"irc060_required_for_physical_scan", "true"});
  stream->evidence.push_back({"delete_row_locator_stream",
                              "table_scan_fallback_no_applicable_locator"});
}

EngineApiDiagnostic DeleteTargetAccessRefusalDiagnostic(
    const DmlTargetAccessPlan& plan) {
  const std::string detail = plan.diagnostics.empty()
                                 ? "target_access_plan_refused"
                                 : "target_access_plan_refused:" +
                                       plan.diagnostics.front();
  return MakeInvalidRequestDiagnostic("dml.delete_rows", detail);
}

DeleteTargetCandidateStream BuildDeleteTargetCandidateStream(
    const EngineDeleteRowsRequest& request,
    const MgaRelationReadView& state,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& visible_indexes) {
  DeleteTargetCandidateStream stream;
  bool unsupported_predicate = false;
  bool unusable_index_present = false;
  const auto plan_request = BuildDeleteTargetAccessPlanRequest(request,
                                                               table,
                                                               visible_indexes,
                                                               &unsupported_predicate,
                                                               &unusable_index_present);
  stream.plan = BuildDmlTargetAccessPlan(plan_request);
  AddTargetAccessPlanEvidence(stream.plan,
                              "delete_target_access_kind",
                              &stream.evidence);

  if (!stream.plan.ok) {
    if (UnsafeTargetAccessRefusal(stream.plan)) {
      stream.fail_closed = true;
      stream.diagnostic = DeleteTargetAccessRefusalDiagnostic(stream.plan);
      stream.evidence.push_back({"delete_row_candidate_stream", "refused"});
      stream.evidence.push_back({"delete_target_access_refusal",
                                 "fail_closed_unsafe_route"});
      return stream;
    }
    AddDeleteCandidateFallbackEvidence("target_access_plan_refused", &stream);
    return stream;
  }

  switch (stream.plan.access_kind) {
    case DmlTargetAccessKind::row_uuid_singleton: {
      stream.evidence.push_back({"delete_row_candidate_stream", "row_uuid_singleton"});
      const auto locator_stream =
          BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::delete_row,
                                  stream.plan);
      AppendDmlRowLocatorStreamEvidence("delete", locator_stream, &stream.evidence);
      if (!locator_stream.ok) {
        stream.fail_closed = true;
        stream.diagnostic = locator_stream.diagnostic;
        return stream;
      }
      const auto row = FindVisibleRowUuidCandidate(state,
                                                   table.table_uuid,
                                                   stream.plan.row_uuid,
                                                   request.context);
      if (row && CrudRowMatchesPredicate(*row, request.delete_predicate)) {
        stream.rows.push_back(*row);
        AddDmlHotPointAdmissionEvidence(plan_request,
                                        row->row_uuid,
                                        &stream.evidence);
      }
      stream.rows_ready = true;
      return stream;
    }
    case DmlTargetAccessKind::row_uuid_list: {
      stream.evidence.push_back({"delete_row_candidate_stream", "row_uuid_list"});
      stream.evidence.push_back({"delete_row_uuid_list_size",
                                 std::to_string(stream.plan.row_uuids.size())});
      const auto locator_stream =
          BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::delete_row,
                                  stream.plan);
      AppendDmlRowLocatorStreamEvidence("delete", locator_stream, &stream.evidence);
      if (!locator_stream.ok) {
        stream.fail_closed = true;
        stream.diagnostic = locator_stream.diagnostic;
        return stream;
      }
      stream.rows.reserve(stream.plan.row_uuids.size());
      const auto candidate_rows =
          FindVisibleRowUuidCandidates(state,
                                       table.table_uuid,
                                       stream.plan.row_uuids,
                                       request.context);
      stream.evidence.push_back({"delete_row_uuid_list_lookup",
                                 "single_pass_mga_visibility"});
      for (const auto& row : candidate_rows) {
        if (CrudRowMatchesPredicate(row, request.delete_predicate)) {
          stream.rows.push_back(row);
          AddDmlHotPointAdmissionEvidence(plan_request,
                                          row.row_uuid,
                                          &stream.evidence);
        }
      }
      stream.rows_ready = true;
      return stream;
    }
    case DmlTargetAccessKind::unique_index_lookup:
    case DmlTargetAccessKind::nonunique_index_lookup:
    case DmlTargetAccessKind::range_index_lookup: {
      const auto indexed = IndexedMgaRowsForPredicateForContext(state,
                                                                table.table_uuid,
                                                                request.delete_predicate,
                                                                request.context,
                                                                0);
      stream.evidence.insert(stream.evidence.end(),
                             indexed.evidence.begin(),
                             indexed.evidence.end());
      if (indexed.index_used) {
        stream.rows = indexed.rows;
        if (stream.rows.empty()) {
          stream.rows = ScanVisibleRowsMatchingPredicate(state,
                                                         table.table_uuid,
                                                         request.context,
                                                         request.delete_predicate,
                                                         0);
          stream.evidence.push_back(
              {"delete_index_empty_visible_scan_fallback",
               stream.rows.empty() ? "no_visible_match"
                                   : "matched_visible_rows"});
          stream.evidence.push_back(
              {"delete_index_empty_visible_scan_fallback_rows",
               std::to_string(stream.rows.size())});
        }
        stream.rows_ready = true;
        stream.evidence.push_back({"delete_row_candidate_stream", "indexed_predicate"});
        stream.evidence.push_back({"index_lookup", indexed.index_evidence_id});
        stream.evidence.push_back({"physical_index_tree_available", "false"});
        stream.evidence.push_back({"irc060_required_for_physical_scan", "true"});
        const auto locator_plan =
            BuildRowUuidLocatorPlanFromRows(plan_request, stream.rows);
        const auto locator_stream =
            BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::delete_row,
                                    locator_plan);
        AppendDmlRowLocatorStreamEvidence("delete", locator_stream, &stream.evidence);
        if (!locator_stream.ok) {
          stream.fail_closed = true;
          stream.diagnostic = locator_stream.diagnostic;
          return stream;
        }
        stream.evidence.push_back({"delete_row_locator_stream",
                                   "consumed_row_uuid_after_index_probe"});
        for (const auto& row : stream.rows) {
          AddDmlHotPointAdmissionEvidence(plan_request,
                                          row.row_uuid,
                                          &stream.evidence);
        }
        return stream;
      }
      if (indexed.index_refused) {
        stream.fail_closed = true;
        stream.diagnostic =
            indexed.diagnostic.detail.empty()
                ? MakeInvalidRequestDiagnostic("dml.delete_rows",
                                               "mga_indexed_lookup_refused")
                : indexed.diagnostic;
        stream.evidence.push_back({"delete_row_candidate_stream", "refused"});
        stream.evidence.push_back({"delete_target_access_index_refusal",
                                   indexed.diagnostic.detail.empty()
                                       ? indexed.diagnostic.message_key
                                       : indexed.diagnostic.detail});
        stream.evidence.push_back({"delete_target_access_refusal",
                                   "fail_closed_index_locator_stream"});
        return stream;
      } else {
        stream.fail_closed = true;
        stream.diagnostic = MakeInvalidRequestDiagnostic(
            "dml.delete_rows",
            "planned_index_lookup_not_used");
        stream.evidence.push_back({"delete_row_candidate_stream", "refused"});
        stream.evidence.push_back({"delete_target_access_refusal",
                                   "planned_index_lookup_not_used"});
        return stream;
      }
    }
    case DmlTargetAccessKind::table_scan:
      {
        const auto locator_stream =
            BuildRouteLocatorStream(DmlRowLocatorStreamConsumer::delete_row,
                                    stream.plan,
                                    true,
                                    false);
        AppendDmlRowLocatorStreamEvidence("delete", locator_stream, &stream.evidence);
        if (!locator_stream.ok) {
          stream.fail_closed = true;
          stream.diagnostic = locator_stream.diagnostic;
          return stream;
        }
      }
      AddDeleteCandidateFallbackEvidence(
          unsupported_predicate ? "unsupported predicate"
                                : (unusable_index_present ? "unusable index"
                                                          : "unindexed predicate"),
          &stream);
      return stream;
    case DmlTargetAccessKind::summary_pruned:
    case DmlTargetAccessKind::refused:
      AddDeleteCandidateFallbackEvidence("unsupported predicate", &stream);
      return stream;
  }
  AddDeleteCandidateFallbackEvidence("unsupported predicate", &stream);
  return stream;
}

// DPC_HOT_UPDATE_SHAPE
struct HotUpdateIndexDisciplineCounters {
  std::uint64_t index_churn_avoided = 0;
  std::uint64_t exact_secondary_churn_avoided = 0;
  std::uint64_t synchronous_changed_key_maintained = 0;
  std::uint64_t synchronous_unchanged_key_skipped = 0;
  std::uint64_t deferred_changed_key_delta_pairs = 0;
  std::uint64_t deferred_unchanged_key_skipped = 0;
  std::uint64_t disabled_baseline_churn_decisions = 0;
  std::uint64_t page_local_hot_updates = 0;
  std::uint64_t stable_row_head_indirection_updates = 0;
  std::uint64_t ordinary_index_rewrite_updates = 0;
  std::uint64_t mga_visibility_proof_accepted = 0;
  std::uint64_t mga_visibility_proof_refused = 0;
};

bool HotUpdateShapeEnabled(const EngineUpdateRowsRequest& request) {
  for (const auto& option : request.option_envelopes) {
    if (option == "runtime.hot_update_shape=disabled" ||
        option == "runtime.hot_update_shape=false" ||
        option == "SCRATCHBIRD_HOT_UPDATE_SHAPE=0" ||
        option == "SCRATCHBIRD_HOT_UPDATE_SHAPE=false" ||
        option == "SCRATCHBIRD_HOT_UPDATE_SHAPE=disabled") {
      return false;
    }
    if (option == "runtime.hot_update_shape=enabled" ||
        option == "runtime.hot_update_shape=true" ||
        option == "SCRATCHBIRD_HOT_UPDATE_SHAPE=1" ||
        option == "SCRATCHBIRD_HOT_UPDATE_SHAPE=true" ||
        option == "SCRATCHBIRD_HOT_UPDATE_SHAPE=enabled") {
      return true;
    }
  }
  return true;
}

bool IndexKeysChanged(const CrudIndexRecord& index,
                      const std::vector<std::pair<std::string, std::string>>& before,
                      const std::vector<std::pair<std::string, std::string>>& after) {
  return CrudIndexKeysForValues(index, before) != CrudIndexKeysForValues(index, after);
}

EngineApiDiagnostic DiagnosticFromMgaRecord(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const std::string& fallback_code,
    const std::string& fallback_key) {
  std::string detail = diagnostic.remediation_hint;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) { detail += ";"; }
    detail += argument.key + "=" + argument.value;
  }
  return MakeEngineApiDiagnostic(diagnostic.diagnostic_code.empty()
                                     ? fallback_code
                                     : diagnostic.diagnostic_code,
                                 diagnostic.message_key.empty()
                                     ? fallback_key
                                     : diagnostic.message_key,
                                 detail,
                                 true);
}

mga::HotStableRowHeadDecisionResult OrdinaryHotPlusDecision() {
  mga::HotStableRowHeadDecisionResult result;
  result.decision = mga::HotStableRowHeadDecisionKind::ordinary_index_rewrite;
  return result;
}

bool ParserOrReferenceAuthorityForHotProof(const EngineUpdateRowsRequest& request) {
  return request.update_predicate.predicate_kind == "reference_bulk" ||
         UpdateOptionEnabled(request, "odf031.parser_or_reference_authority=true");
}

std::uint64_t HotPlusSamePageBudgetBytes(const EngineUpdateRowsRequest& request) {
  std::uint64_t budget = kCrudVerticalSliceMaxEncodedValueBytes;
  budget = UpdateOptionU64(request,
                           "runtime.hot_plus_same_page_budget_bytes=",
                           budget);
  budget = UpdateOptionU64(request,
                           "runtime.hot_plus.same_page_budget_bytes=",
                           budget);
  return budget;
}

bool HotPlusSamePageBudgetAvailable(
    const EngineUpdateRowsRequest& request,
    const std::vector<std::pair<std::string, std::string>>& values,
    bool toast_required) {
  if (toast_required) {
    return false;
  }
  return static_cast<std::uint64_t>(EncodedValueBytes(values)) <=
         HotPlusSamePageBudgetBytes(request);
}

bool HotPlusSamePageBudgetAvailableForEncodedBytes(
    const EngineUpdateRowsRequest& request,
    std::size_t encoded_bytes,
    bool toast_required) {
  if (toast_required) {
    return false;
  }
  return static_cast<std::uint64_t>(encoded_bytes) <=
         HotPlusSamePageBudgetBytes(request);
}

bool IsSynchronousUpdateIndexAction(UpdateIndexMaintenanceAction action) {
  return action == UpdateIndexMaintenanceAction::synchronous_exact_rewrite ||
         action == UpdateIndexMaintenanceAction::synchronous_exact_probe_then_rewrite;
}

bool UpdateIndexActionNeedsKeyState(UpdateIndexMaintenanceAction action) {
  return IsSynchronousUpdateIndexAction(action) ||
         action == UpdateIndexMaintenanceAction::committed_delta_ledger;
}

bool UpdatePlanHasMaintainableIndexWork(
    const UpdateBatchContext& batch_context) {
  for (const auto& entry : batch_context.index_plan.entries) {
    if (IsSynchronousUpdateIndexAction(entry.action) ||
        entry.action == UpdateIndexMaintenanceAction::committed_delta_ledger) {
      return true;
    }
  }
  return false;
}

std::vector<StagedUpdateRow::IndexKeyState> BuildStagedUpdateIndexKeyStates(
    const UpdateBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const std::vector<std::pair<std::string, std::string>>& new_values) {
  std::vector<StagedUpdateRow::IndexKeyState> states;
  states.resize(batch_context.index_plan.entries.size());
  for (std::size_t index = 0; index < batch_context.index_plan.entries.size(); ++index) {
    const auto& entry = batch_context.index_plan.entries[index];
    if (!UpdateIndexActionNeedsKeyState(entry.action)) {
      continue;
    }
    auto& state = states[index];
    state.old_keys = CrudIndexKeysForValues(entry.index, old_row.values);
    state.new_keys = CrudIndexKeysForValues(entry.index, new_values);
    state.keys_changed = state.old_keys != state.new_keys;
    state.materialized = true;
  }
  return states;
}

const StagedUpdateRow::IndexKeyState* StagedUpdateIndexKeyStateAt(
    const std::vector<StagedUpdateRow::IndexKeyState>* states,
    std::size_t index) {
  if (states == nullptr || index >= states->size()) {
    return nullptr;
  }
  const auto& state = (*states)[index];
  return state.materialized ? &state : nullptr;
}

bool HotPlusExactIndexKeysUnchanged(
    const UpdateBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const std::vector<std::pair<std::string, std::string>>& new_values,
    const std::vector<StagedUpdateRow::IndexKeyState>* index_key_states) {
  for (std::size_t index = 0; index < batch_context.index_plan.entries.size(); ++index) {
    const auto& entry = batch_context.index_plan.entries[index];
    if (!IsSynchronousUpdateIndexAction(entry.action) &&
        entry.action != UpdateIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    const auto* key_state = StagedUpdateIndexKeyStateAt(index_key_states, index);
    const bool keys_changed =
        key_state == nullptr
            ? IndexKeysChanged(entry.index, old_row.values, new_values)
            : key_state->keys_changed;
    if (keys_changed) {
      return false;
    }
  }
  return true;
}

mga::TransactionState RowVersionStateToCreatorState(
    const mga::TransactionInventoryEntry& entry) {
  return entry.state;
}

mga::RowVersionState RowVersionStateForCreator(
    const mga::TransactionInventoryEntry& entry,
    bool deleted) {
  if (deleted) {
    return mga::RowVersionState::delete_marker;
  }
  switch (entry.state) {
    case mga::TransactionState::active:
      return mga::RowVersionState::uncommitted;
    case mga::TransactionState::preparing:
    case mga::TransactionState::prepared:
      return mga::RowVersionState::prepared;
    case mga::TransactionState::committed:
    case mga::TransactionState::archived:
      return mga::RowVersionState::committed;
    case mga::TransactionState::rolling_back:
    case mga::TransactionState::rolled_back:
    case mga::TransactionState::failed_terminal:
      return mga::RowVersionState::rolled_back;
    case mga::TransactionState::limbo:
      return mga::RowVersionState::limbo;
    case mga::TransactionState::recovering:
      return mga::RowVersionState::recovery_required;
    case mga::TransactionState::none:
    case mga::TransactionState::created:
    case mga::TransactionState::read_only_active:
    default:
      return mga::RowVersionState::unknown;
  }
}

scratchbird::core::platform::TypedUuid ParseHotProofUuid(
    scratchbird::core::platform::UuidKind kind,
    const std::string& text) {
  const auto parsed = uuid::ParseDurableEngineIdentityUuid(kind, text);
  return parsed.ok() ? parsed.value : scratchbird::core::platform::TypedUuid{};
}

struct HotProofTransactionLookup {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  mga::TransactionInventoryEntry entry;
};

HotProofTransactionLookup LookupHotProofTransaction(
    const mga::LocalTransactionInventory& inventory,
    std::uint64_t local_transaction_id) {
  HotProofTransactionLookup result;
  if (local_transaction_id == 0) {
    result.diagnostic =
        MakeInvalidRequestDiagnostic("dml.update_rows.hot_plus",
                                     "local_transaction_id_required");
    return result;
  }
  const auto lookup =
      mga::LookupLocalTransaction(inventory,
                                  mga::MakeLocalTransactionId(local_transaction_id));
  if (!lookup.ok()) {
    result.diagnostic = DiagnosticFromMgaRecord(
        lookup.diagnostic,
        "SB-MGA-HOT-STABLE-HEAD-TXN-LOOKUP-FAILED",
        "row_version.hot_stable_head.transaction_lookup_failed");
    return result;
  }
  result.ok = true;
  result.entry = lookup.entry;
  return result;
}

mga::RowVersionMetadata MakeHotProofRowMetadata(
    const CrudRowVersionRecord& row,
    const mga::TransactionInventoryEntry& creator,
    std::uint64_t version_sequence,
    mga::RowVersionState row_state,
    mga::TransactionState creator_state,
    const scratchbird::core::platform::TypedUuid& row_uuid,
    const scratchbird::core::platform::TypedUuid& previous_version_uuid,
    std::uint64_t previous_sequence) {
  mga::RowVersionMetadata metadata;
  metadata.identity.row.row_uuid = row_uuid;
  metadata.identity.creator_transaction = creator.identity;
  metadata.identity.version_sequence = version_sequence;
  metadata.chain.previous_version_uuid = previous_version_uuid;
  metadata.chain.previous_version_sequence = previous_sequence;
  metadata.state = row_state;
  metadata.creator_transaction_state = creator_state;
  metadata.payload_present = !row.deleted && !row.values.empty();
  return metadata;
}

struct HotPlusDecisionBuildResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  mga::HotStableRowHeadDecisionResult decision;
};

HotPlusDecisionBuildResult BuildHotPlusDecisionForStagedUpdate(
    const EngineUpdateRowsRequest& request,
    const mga::LocalTransactionInventory& inventory,
    const UpdateBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const CrudRowVersionRecord& new_row,
    const std::vector<std::pair<std::string, std::string>>& new_values,
    const std::vector<StagedUpdateRow::IndexKeyState>* index_key_states,
    std::size_t new_values_encoded_bytes,
    bool toast_required,
    bool hot_update_shape_enabled) {
  HotPlusDecisionBuildResult result;
  if (!hot_update_shape_enabled) {
    result.ok = true;
    result.decision = OrdinaryHotPlusDecision();
    return result;
  }

  const bool parser_or_reference_authority =
      ParserOrReferenceAuthorityForHotProof(request);
  const bool exact_index_keys_unchanged =
      HotPlusExactIndexKeysUnchanged(batch_context,
                                     old_row,
                                     new_values,
                                     index_key_states);
  if (!parser_or_reference_authority && !exact_index_keys_unchanged) {
    result.ok = true;
    result.decision = OrdinaryHotPlusDecision();
    return result;
  }

  const auto old_creator =
      LookupHotProofTransaction(inventory, old_row.creator_tx);
  if (!old_creator.ok) {
    result.diagnostic = old_creator.diagnostic;
    return result;
  }
  const auto new_creator =
      LookupHotProofTransaction(inventory, request.context.local_transaction_id);
  if (!new_creator.ok) {
    result.diagnostic = new_creator.diagnostic;
    return result;
  }

  const auto old_row_uuid =
      ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
                        old_row.row_uuid);
  const auto old_version_uuid =
      ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
                        old_row.version_uuid);
  const auto new_row_uuid =
      new_row.row_uuid == old_row.row_uuid
          ? old_row_uuid
          : ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
                              new_row.row_uuid);
  const auto new_previous_version_uuid =
      new_row.previous_version_uuid == old_row.version_uuid
          ? old_version_uuid
          : ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
                              new_row.previous_version_uuid);
  const auto old_previous_version_uuid =
      old_row.previous_version_uuid.empty()
          ? scratchbird::core::platform::TypedUuid{}
          : ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
                              old_row.previous_version_uuid);

  const std::uint64_t proof_new_sequence =
      old_row.sequence == 0 ? 1 : old_row.sequence + 1;

  mga::HotStableRowHeadProofInput input;
  input.old_visible_version = MakeHotProofRowMetadata(
      old_row,
      old_creator.entry,
      old_row.sequence,
      RowVersionStateForCreator(old_creator.entry, old_row.deleted),
      RowVersionStateToCreatorState(old_creator.entry),
      old_row_uuid,
      old_previous_version_uuid,
      old_row.previous_sequence);
  input.new_version = MakeHotProofRowMetadata(
      new_row,
      new_creator.entry,
      proof_new_sequence,
      mga::RowVersionState::uncommitted,
      mga::TransactionState::active,
      new_row_uuid,
      new_previous_version_uuid,
      new_row.previous_sequence);
  input.old_version_uuid = old_version_uuid;
  input.new_previous_version_uuid = new_previous_version_uuid;
  input.visibility_snapshot.reader_transaction =
      mga::MakeLocalTransactionId(request.context.local_transaction_id);
  input.visibility_snapshot.visible_through_local_transaction_id =
      request.context.snapshot_visible_through_local_transaction_id;
  input.visibility_snapshot.allow_reader_own_uncommitted = true;
  input.exact_index_keys_unchanged = exact_index_keys_unchanged;
  input.same_page_budget_available =
      HotPlusSamePageBudgetAvailableForEncodedBytes(
          request,
          new_values_encoded_bytes,
          toast_required);
  input.parser_or_reference_authority = parser_or_reference_authority;

  result.ok = true;
  result.decision = mga::EvaluateHotStableRowHeadDecision(input);
  return result;
}

bool HotPlusDecisionAvoidsExactChurn(
    const mga::HotStableRowHeadDecisionResult& decision) {
  return decision.proof_accepted &&
         (decision.decision ==
              mga::HotStableRowHeadDecisionKind::page_local_hot ||
          decision.decision ==
              mga::HotStableRowHeadDecisionKind::stable_row_head_indirection);
}

void RecordHotPlusDecisionCounter(
    const mga::HotStableRowHeadDecisionResult& decision,
    HotUpdateIndexDisciplineCounters* counters) {
  if (counters == nullptr) {
    return;
  }
  if (!decision.ok()) {
    ++counters->mga_visibility_proof_refused;
    return;
  }
  if (decision.proof_accepted) {
    ++counters->mga_visibility_proof_accepted;
  }
  switch (decision.decision) {
    case mga::HotStableRowHeadDecisionKind::page_local_hot:
      ++counters->page_local_hot_updates;
      break;
    case mga::HotStableRowHeadDecisionKind::stable_row_head_indirection:
      ++counters->stable_row_head_indirection_updates;
      break;
    case mga::HotStableRowHeadDecisionKind::ordinary_index_rewrite:
      ++counters->ordinary_index_rewrite_updates;
      break;
    case mga::HotStableRowHeadDecisionKind::refused:
      ++counters->mga_visibility_proof_refused;
      break;
  }
}

std::uint64_t CountUnaffectedExactIndexChurnAvoided(
    const UpdateBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const std::vector<std::pair<std::string, std::string>>& new_values,
    const mga::HotStableRowHeadDecisionResult& decision,
    const std::vector<StagedUpdateRow::IndexKeyState>* index_key_states) {
  if (!HotPlusDecisionAvoidsExactChurn(decision)) {
    return 0;
  }
  std::uint64_t avoided = 0;
  for (std::size_t index = 0; index < batch_context.index_plan.entries.size(); ++index) {
    const auto& entry = batch_context.index_plan.entries[index];
    if (entry.action != UpdateIndexMaintenanceAction::unaffected) {
      continue;
    }
    const auto* key_state = StagedUpdateIndexKeyStateAt(index_key_states, index);
    const bool keys_changed =
        key_state == nullptr
            ? IndexKeysChanged(entry.index, old_row.values, new_values)
            : key_state->keys_changed;
    if (keys_changed) {
      continue;
    }
    avoided += static_cast<std::uint64_t>(
        key_state == nullptr
            ? CrudIndexKeysForValues(entry.index, old_row.values).size()
            : key_state->old_keys.size());
  }
  return avoided;
}

bool ShouldMaintainUpdateIndex(const UpdateIndexMaintenancePlanEntry& entry,
                               std::size_t entry_index,
                               const CrudRowVersionRecord& old_row,
                               const std::vector<std::pair<std::string, std::string>>& new_values,
                               bool hot_update_shape_enabled,
                               const mga::HotStableRowHeadDecisionResult& hot_plus_decision,
                               const std::vector<StagedUpdateRow::IndexKeyState>* index_key_states) {
  if (!IsSynchronousUpdateIndexAction(entry.action) &&
      entry.action != UpdateIndexMaintenanceAction::committed_delta_ledger) {
    return false;
  }
  if (!hot_update_shape_enabled) {
    return true;
  }
  const auto* key_state = StagedUpdateIndexKeyStateAt(index_key_states, entry_index);
  const bool keys_changed =
      key_state == nullptr
          ? IndexKeysChanged(entry.index, old_row.values, new_values)
          : key_state->keys_changed;
  if (!keys_changed && HotPlusDecisionAvoidsExactChurn(hot_plus_decision)) {
    return false;
  }
  return true;
}

std::uint64_t PlannedUpdateIndexMaintenanceWrites(
    const UpdateBatchContext& batch_context,
    const std::vector<StagedUpdateRow>& staged_update_rows,
    bool hot_update_shape_enabled,
    std::string* first_index_uuid) {
  if (!UpdatePlanHasMaintainableIndexWork(batch_context)) {
    return 0;
  }
  std::uint64_t planned_writes = 0;
  for (std::size_t entry_index = 0; entry_index < batch_context.index_plan.entries.size(); ++entry_index) {
    const auto& entry = batch_context.index_plan.entries[entry_index];
    for (const auto& staged : staged_update_rows) {
      if (!ShouldMaintainUpdateIndex(entry,
                                     entry_index,
                                     staged.original_row,
                                     staged.logical_values,
                                     hot_update_shape_enabled,
                                     staged.hot_plus_decision,
                                     &staged.index_key_states)) {
        continue;
      }
      if (first_index_uuid != nullptr && first_index_uuid->empty()) {
        *first_index_uuid = entry.index.index_uuid;
      }
      const auto* key_state =
          StagedUpdateIndexKeyStateAt(&staged.index_key_states, entry_index);
      if (entry.action == UpdateIndexMaintenanceAction::committed_delta_ledger) {
        planned_writes += static_cast<std::uint64_t>(
            key_state == nullptr
                ? CrudIndexKeysForValues(entry.index,
                                         staged.original_row.values).size()
                : key_state->old_keys.size());
      }
      planned_writes += static_cast<std::uint64_t>(
          key_state == nullptr
              ? CrudIndexKeysForValues(entry.index,
                                       staged.logical_values).size()
              : key_state->new_keys.size());
    }
  }
  return planned_writes;
}

std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> UpdateDeltaEntries(
    const UpdateBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const CrudRowVersionRecord& new_row,
    const std::vector<std::pair<std::string, std::string>>& new_values,
    const std::vector<StagedUpdateRow::IndexKeyState>* index_key_states,
    bool hot_update_shape_enabled,
    const mga::HotStableRowHeadDecisionResult& hot_plus_decision,
    HotUpdateIndexDisciplineCounters* counters) {
  // DPC_DEFERRED_INDEX_WRITE_PATH
  // DPC_HOT_UPDATE_SHAPE
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  if (!UpdatePlanHasMaintainableIndexWork(batch_context)) {
    return entries;
  }
  for (std::size_t index = 0; index < batch_context.index_plan.entries.size(); ++index) {
    const auto& entry = batch_context.index_plan.entries[index];
    if (entry.action != UpdateIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    const auto* key_state = StagedUpdateIndexKeyStateAt(index_key_states, index);
    const bool keys_changed =
        key_state == nullptr
            ? IndexKeysChanged(entry.index, old_row.values, new_values)
            : key_state->keys_changed;
    if (hot_update_shape_enabled && !keys_changed &&
        HotPlusDecisionAvoidsExactChurn(hot_plus_decision)) {
      if (counters != nullptr) {
        ++counters->deferred_unchanged_key_skipped;
        ++counters->index_churn_avoided;
        ++counters->exact_secondary_churn_avoided;
      }
      continue;
    }
    if (counters != nullptr) {
      if (keys_changed) {
        ++counters->deferred_changed_key_delta_pairs;
      } else {
        ++counters->disabled_baseline_churn_decisions;
      }
    }
    MgaSecondaryIndexDeltaLedgerEntryInput before;
    before.index = entry.index;
    before.table_uuid = batch_context.target_object_uuid;
    before.row_uuid = old_row.row_uuid;
    before.version_uuid = old_row.version_uuid;
    before.values = old_row.values;
    before.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::update_before;
    before.source_evidence_reference =
        "engine.dml.update.secondary_index_delta_before:" + batch_context.statement_uuid;
    entries.push_back(std::move(before));

    MgaSecondaryIndexDeltaLedgerEntryInput after;
    after.index = entry.index;
    after.table_uuid = batch_context.target_object_uuid;
    after.row_uuid = new_row.row_uuid;
    after.version_uuid = new_row.version_uuid;
    after.values = new_values;
    after.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::update_after;
    after.source_evidence_reference =
        "engine.dml.update.secondary_index_delta_after:" + batch_context.statement_uuid;
    entries.push_back(std::move(after));
  }
  return entries;
}

EngineApiDiagnostic AppendSynchronousUpdateIndexEntries(
    const EngineRequestContext& context,
    const UpdateBatchContext& batch_context,
    const std::string& table_uuid,
    const std::vector<StagedUpdateRow>& staged_update_rows,
    const std::vector<CrudRowVersionRecord>& row_records,
    bool hot_update_shape_enabled,
    MgaRelationHotAppendContext* append_context,
    HotUpdateIndexDisciplineCounters* counters,
    std::vector<EngineEvidenceReference>* evidence) {
  // DPC_HOT_UPDATE_SHAPE: synchronous index maintenance is now based on the
  // actual old/new key comparison for each row version, while preserving the
  // append-plus-visible-row-recheck model used for key-changing updates.
  std::vector<DmlTransactionalIndexEntryRequest> retire_requests;
  std::vector<DmlTransactionalIndexEntryRequest> insert_requests;
  if (!UpdatePlanHasMaintainableIndexWork(batch_context)) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  for (std::size_t entry_index = 0; entry_index < batch_context.index_plan.entries.size(); ++entry_index) {
    const auto& entry = batch_context.index_plan.entries[entry_index];
    if (!IsSynchronousUpdateIndexAction(entry.action) &&
        entry.action !=
            UpdateIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    if (!IsAdmittedMgaTransactionalIndexFamily(entry.index)) {
      return MakeInvalidRequestDiagnostic(
          "dml.update_rows.index_maintenance",
          "synchronous_index_family_has_no_transactional_provider:" +
              (entry.index.family.empty()
                   ? CrudIndexFamilyForProfile(entry.index.profile)
                   : entry.index.family));
    }
    for (std::size_t index = 0; index < staged_update_rows.size(); ++index) {
      const auto& staged = staged_update_rows[index];
      const auto* key_state =
          StagedUpdateIndexKeyStateAt(&staged.index_key_states, entry_index);
      const bool keys_changed =
          key_state == nullptr
              ? IndexKeysChanged(entry.index,
                                 staged.original_row.values,
                                 staged.logical_values)
              : key_state->keys_changed;
      if (hot_update_shape_enabled && !keys_changed &&
          HotPlusDecisionAvoidsExactChurn(staged.hot_plus_decision)) {
        if (counters != nullptr) {
          ++counters->synchronous_unchanged_key_skipped;
          ++counters->index_churn_avoided;
          ++counters->exact_secondary_churn_avoided;
        }
        continue;
      }
      if (counters != nullptr) {
        if (keys_changed) {
          ++counters->synchronous_changed_key_maintained;
        } else {
          ++counters->disabled_baseline_churn_decisions;
        }
      }
      std::vector<std::string> fallback_old_keys;
      std::vector<std::string> fallback_new_keys;
      const std::vector<std::string>* old_keys =
          key_state == nullptr ? nullptr : &key_state->old_keys;
      const std::vector<std::string>* new_keys =
          key_state == nullptr ? nullptr : &key_state->new_keys;
      if (old_keys == nullptr) {
        fallback_old_keys =
            CrudIndexKeysForValues(entry.index, staged.original_row.values);
        old_keys = &fallback_old_keys;
      }
      if (new_keys == nullptr) {
        fallback_new_keys =
            CrudIndexKeysForValues(entry.index, staged.logical_values);
        new_keys = &fallback_new_keys;
      }
      if (*old_keys != *new_keys) {
        const std::string old_payload = CrudFieldValue(
            staged.original_row.values, entry.index.column_name);
        for (const auto& key : *old_keys) {
          retire_requests.push_back(
              {entry.index,
               table_uuid,
               row_records[index].row_uuid,
               row_records[index].version_uuid,
               staged.original_row.version_uuid,
               key,
               old_payload});
        }
      }
      const std::string new_payload = CrudFieldValue(
          staged.logical_values, entry.index.column_name);
      for (const auto& key : *new_keys) {
        insert_requests.push_back(
            {entry.index,
             table_uuid,
             row_records[index].row_uuid,
             row_records[index].version_uuid,
             staged.original_row.version_uuid,
             key,
             new_payload});
      }
    }
  }
  if (!retire_requests.empty() || !insert_requests.empty()) {
    TransactionalRelationStore relation_store(context);
    auto local_append_context = append_context == nullptr
                                    ? relation_store.OpenHotAppendContext()
                                    : MgaRelationHotAppendContext(context);
    MgaRelationHotAppendContext* target_context =
        append_context == nullptr ? &local_append_context : append_context;
    MgaTransactionalIndexProvider provider(context, target_context);
    const auto retired = provider.PrepareRetireEntries(retire_requests);
    if (!retired.ok) return retired.diagnostic;
    const auto inserted = provider.PrepareInsertEntries(insert_requests);
    if (!inserted.ok) return inserted.diagnostic;
    if (evidence != nullptr) {
      evidence->insert(evidence->end(), retired.evidence.begin(),
                       retired.evidence.end());
      evidence->insert(evidence->end(), inserted.evidence.begin(),
                       inserted.evidence.end());
      evidence->push_back({"update_index_apply",
                           "transactional_provider_exact_key_cache_reuse"});
      evidence->push_back({"update_index_apply_exact_entry_count",
                           std::to_string(insert_requests.size())});
      evidence->push_back({"update_index_apply_retire_entry_count",
                           std::to_string(retire_requests.size())});
    }
    if (append_context == nullptr) {
      return local_append_context.FlushIndexEntries();
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

void AddHotUpdateIndexDisciplineEvidence(bool hot_update_shape_enabled,
                                         const HotUpdateIndexDisciplineCounters& counters,
                                         EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({"DPC_HOT_UPDATE_SHAPE", "version_chain_index_discipline"});
  result->evidence.push_back({"dpc_hot_update_shape_runtime",
                              hot_update_shape_enabled ? "enabled" : "disabled_baseline"});
  result->evidence.push_back({"dpc_hot_update_shape_index_churn_avoided",
                              std::to_string(counters.index_churn_avoided)});
  result->evidence.push_back({"hot_plus_exact_secondary_churn_avoided",
                              std::to_string(counters.exact_secondary_churn_avoided)});
  result->evidence.push_back({"hot_plus_page_local_hot_updates",
                              std::to_string(counters.page_local_hot_updates)});
  result->evidence.push_back({"hot_plus_stable_row_head_indirection_updates",
                              std::to_string(counters.stable_row_head_indirection_updates)});
  result->evidence.push_back({"hot_plus_ordinary_index_rewrite_updates",
                              std::to_string(counters.ordinary_index_rewrite_updates)});
  result->evidence.push_back({"hot_plus_mga_visibility_proof_accepted",
                              std::to_string(counters.mga_visibility_proof_accepted)});
  result->evidence.push_back({"hot_plus_mga_visibility_proof_refused",
                              std::to_string(counters.mga_visibility_proof_refused)});
  result->evidence.push_back({"dpc_hot_update_shape_synchronous_changed_key_maintained",
                              std::to_string(counters.synchronous_changed_key_maintained)});
  result->evidence.push_back({"dpc_hot_update_shape_synchronous_unchanged_key_skipped",
                              std::to_string(counters.synchronous_unchanged_key_skipped)});
  result->evidence.push_back({"dpc_hot_update_shape_deferred_changed_key_delta_pairs",
                              std::to_string(counters.deferred_changed_key_delta_pairs)});
  result->evidence.push_back({"dpc_hot_update_shape_deferred_unchanged_key_skipped",
                              std::to_string(counters.deferred_unchanged_key_skipped)});
  result->evidence.push_back({"dpc_hot_update_shape_disabled_baseline_churn_decisions",
                              std::to_string(counters.disabled_baseline_churn_decisions)});
}

std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> DeleteDeltaEntries(
    const DeleteBatchContext& batch_context,
    const CrudRowVersionRecord& tombstone_row,
    const CrudRowVersionRecord& original_row) {
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action != DeleteIndexMaintenanceAction::tombstone_delta_ledger) {
      continue;
    }
    MgaSecondaryIndexDeltaLedgerEntryInput input;
    input.index = entry.index;
    input.table_uuid = batch_context.target_object_uuid;
    input.row_uuid = tombstone_row.row_uuid;
    input.version_uuid = tombstone_row.version_uuid;
    input.values = original_row.values;
    input.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::delete_row;
    input.source_evidence_reference =
        "engine.dml.delete.secondary_index_delta:" + batch_context.statement_uuid;
    entries.push_back(std::move(input));
  }
  return entries;
}

EngineApiDiagnostic PrepareSynchronousDeleteIndexRetires(
    const EngineRequestContext& context,
    const DeleteBatchContext& batch_context,
    const std::vector<StagedDeleteRow>& staged_rows,
    const std::vector<CrudRowVersionRecord>& tombstone_rows,
    MgaRelationHotAppendContext* append_context,
    std::vector<EngineEvidenceReference>* evidence) {
  std::vector<DmlTransactionalIndexEntryRequest> requests;
  for (const auto& plan_entry : batch_context.index_plan.entries) {
    if (plan_entry.action !=
            DeleteIndexMaintenanceAction::synchronous_tombstone_rewrite &&
        plan_entry.action !=
            DeleteIndexMaintenanceAction::tombstone_delta_ledger) {
      continue;
    }
    if (!IsAdmittedMgaTransactionalIndexFamily(plan_entry.index)) {
      return MakeInvalidRequestDiagnostic(
          "dml.delete_rows.index_maintenance",
          "synchronous_index_family_has_no_transactional_provider");
    }
    for (std::size_t row_index = 0; row_index < staged_rows.size();
         ++row_index) {
      const auto& old_row = staged_rows[row_index].original_row;
      const auto& tombstone = tombstone_rows[row_index];
      const std::string payload =
          CrudFieldValue(old_row.values, plan_entry.index.column_name);
      for (const auto& key :
           CrudIndexKeysForValues(plan_entry.index, old_row.values)) {
        requests.push_back({plan_entry.index,
                            batch_context.target_object_uuid,
                            tombstone.row_uuid,
                            tombstone.version_uuid,
                            old_row.version_uuid,
                            key,
                            payload});
      }
    }
  }
  MgaTransactionalIndexProvider provider(context, append_context);
  const auto prepared = provider.PrepareRetireEntries(requests);
  if (!prepared.ok) return prepared.diagnostic;
  if (evidence != nullptr) {
    evidence->insert(evidence->end(), prepared.evidence.begin(),
                     prepared.evidence.end());
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {},
                                 false);
}

enum class DmlUpdateDescriptorLifecycleV1 : std::uint8_t {
  kLive = 1,
  kExecuting = 2,
  kPrepared = 3,
  kCompleted = 4,
  kFailed = 5,
};

struct DmlUpdateBoundColumnV1 {
  std::string column_uuid;
  std::uint64_t column_generation = 0;
  std::uint32_t ordinal = 0;
  std::string canonical_name_key;
  std::string datatype_descriptor_uuid;
  std::uint64_t datatype_descriptor_generation = 0;
  std::string type_uuid;
  std::uint64_t type_generation = 0;
  std::string codec_id;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;
};

struct DmlUpdateRowsDescriptorRecordV1 {
  EngineDmlUpdateRowsDescriptorRefV1 descriptor_ref;
  std::string operation_uuid;
  std::uint64_t operation_generation = 1;
  std::string statement_receipt_uuid;
  std::uint64_t structural_occurrence_id = 0;
  std::string database_uuid;
  std::string session_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::string statement_snapshot_uuid;
  std::string metadata_snapshot_uuid;
  std::string datatype_catalog_snapshot_uuid;
  std::uint64_t datatype_catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::uint64_t security_epoch = 0;
  std::string security_context_uuid;
  std::uint64_t security_context_generation = 0;
  std::string security_snapshot_uuid;
  std::uint64_t security_generation = 0;
  std::uint64_t catalog_generation_id = 0;
  SblrExecutorAvailabilitySnapshot executor_availability_snapshot;
  std::string relation_uuid;
  std::uint64_t relation_generation = 0;
  std::string relation_occurrence_uuid;
  std::uint64_t relation_occurrence_generation = 0;
  std::string relation_descriptor_uuid;
  std::uint64_t relation_descriptor_generation = 0;
  std::string publication_barrier_uuid;
  std::uint64_t publication_barrier_generation = 1;
  std::string statement_savepoint_name;
  std::string statement_savepoint_uuid;
  std::uint64_t statement_savepoint_generation = 0;
  std::vector<DmlUpdateBoundColumnV1> assignment_columns;
  std::optional<DmlUpdateBoundColumnV1> predicate_column;
  EngineUpdateRowsRequest prepared_request;
  DmlUpdateDescriptorLifecycleV1 lifecycle =
      DmlUpdateDescriptorLifecycleV1::kLive;
  // Set only after the statement publication barrier has crossed without an
  // acknowledged terminal DUJR successor.  A subsequent same-process replay
  // must bypass the volatile descriptor state and recover the exact MGA-owned
  // chain/staged successor, just as a process restart would.
  bool durable_recovery_required = false;
  EngineUpdateRowsResult completed_result;
  std::vector<std::uint8_t> canonical_result_bytes;
  update_wire::TypedUpdateCarrierSet canonical_carriers;
  update_wire::TypedUpdateSecurityPolicySourceVector
      canonical_source_policies;
  update_wire::TypedUpdateSecuritySnapshotProof
      canonical_security_snapshot;
  update_wire::TypedUpdateDatatypeAuthorityVector
      canonical_datatype_authority;
  update_wire::TypedUpdateBuiltinOperatorAuthorityVector
      canonical_operator_authority;
  EngineDmlUpdateDatatypeOperatorBindingResultV1
      datatype_operator_binding;
  EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1
      datatype_operator_authority;
  EngineDmlUpdatePolicyCatalogCaptureResultV1
      policy_catalog_authority;
  EngineDmlUpdateImmutableAuthoritySnapshotV1 immutable_authority_snapshot;
  // Exact MGA-issued identity reserved before DURC/DUDC encoding.  The
  // durable-registry handle and statement-barrier identity are never issued
  // or inferred by the UPDATE consumer.
  MgaDmlUpdateDurableOperationIdentityV1 durable_operation_identity;
  MgaDmlUpdateStatementSavepointAuthorityV1 statement_mga_authority;
  std::uint64_t journal_sequence = 0;
  update_wire::TypedUpdateJournalState latest_journal_state =
      update_wire::TypedUpdateJournalState::bound;
  update_wire::TypedUpdateHash latest_journal_evidence_sha256{};
};

std::mutex g_dml_update_descriptor_mutex;
std::unordered_map<std::string, DmlUpdateRowsDescriptorRecordV1>
    g_dml_update_descriptors;
std::atomic<std::uint64_t> g_dml_update_descriptor_ordinal{1};
std::atomic<EngineDmlUpdateRowsTestFaultPointV1>
    g_dml_update_test_fault_point{EngineDmlUpdateRowsTestFaultPointV1::none};

bool DmlUpdateTypedUuid(std::string_view text,
                        update_wire::TypedUpdateUuid* value) {
  if (value == nullptr) return false;
  const auto parsed = uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || parsed.value.is_nil()) return false;
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
            value->begin());
  return true;
}

std::string DmlUpdateUuidText(const update_wire::TypedUpdateUuid& value) {
  scratchbird::core::platform::Uuid parsed;
  std::copy(value.begin(), value.end(), parsed.bytes.begin());
  return parsed.is_nil() ? std::string{} : uuid::UuidToString(parsed);
}

bool DmlUpdateIssueIdentity(std::string* value) {
  if (value == nullptr) return false;
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto ordinal = g_dml_update_descriptor_ordinal.fetch_add(
      1, std::memory_order_relaxed);
  const auto identity = uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, now + ordinal);
  if (!identity.ok()) return false;
  *value = uuid::UuidToString(identity.value.value);
  return !value->empty();
}

bool DmlUpdateIssueTypedIdentity(update_wire::TypedUpdateUuid* value) {
  std::string text;
  return DmlUpdateIssueIdentity(&text) && DmlUpdateTypedUuid(text, value);
}

EngineApiDiagnostic DmlUpdateDescriptorDiagnostic(std::string code,
                                                  std::string key,
                                                  std::string detail = {}) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail), true);
}

bool DmlUpdateHasTraceTag(const EngineRequestContext& context,
                          std::string_view tag) {
  return std::find(context.trace_tags.begin(), context.trace_tags.end(), tag) !=
         context.trace_tags.end();
}

bool DmlUpdateCancellationRequested(const EngineRequestContext& context) {
  return context.query_cancellation_requested &&
         context.query_cancellation_requested();
}

bool DmlUpdateTakeTestFault(
    EngineDmlUpdateRowsTestFaultPointV1 fault_point) {
  auto expected = fault_point;
  return g_dml_update_test_fault_point.compare_exchange_strong(
      expected, EngineDmlUpdateRowsTestFaultPointV1::none,
      std::memory_order_acq_rel, std::memory_order_acquire);
}

std::string DmlUpdateLowerAscii(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char ch) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  });
  return lowered;
}

std::optional<std::string> DmlUpdateDescriptorField(
    std::string_view descriptor, std::string_view key) {
  std::optional<std::string> value;
  std::size_t begin = 0;
  while (begin <= descriptor.size()) {
    const auto end = descriptor.find(';', begin);
    const auto field = descriptor.substr(
        begin, end == std::string_view::npos ? descriptor.size() - begin
                                             : end - begin);
    if (field.size() > key.size() && field[key.size()] == '=' &&
        field.substr(0, key.size()) == key) {
      if (value.has_value()) return std::nullopt;
      value = std::string(field.substr(key.size() + 1));
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return value;
}

bool DmlUpdateContextMatches(const EngineRequestContext& context,
                             const DmlUpdateRowsDescriptorRecordV1& record) {
  return context.security_context_present &&
         context.statement_metadata_snapshot_engine_owned &&
         context.statement_receipt_uuid.canonical ==
             record.statement_receipt_uuid &&
         context.database_uuid.canonical == record.database_uuid &&
         context.transaction_uuid.canonical == record.transaction_uuid &&
         context.local_transaction_id == record.local_transaction_id &&
         context.statement_snapshot_uuid.canonical ==
             record.statement_snapshot_uuid &&
         context.statement_metadata_snapshot_uuid.canonical ==
             record.metadata_snapshot_uuid &&
         context.datatype_catalog_snapshot_uuid.canonical ==
             record.datatype_catalog_snapshot_uuid &&
         context.datatype_catalog_generation ==
             record.datatype_catalog_generation &&
         context.datatype_registry_generation ==
             record.datatype_registry_generation &&
         context.security_epoch == record.security_epoch &&
         context.authorization_context.present &&
         context.authorization_context.authority_uuid.canonical ==
             record.security_context_uuid &&
         context.authorization_context.security_context_generation ==
             record.security_context_generation &&
         context.catalog_generation_id == record.catalog_generation_id;
}

SblrExecutorAvailabilityRowIdentity DmlUpdateRowsExecutorIdentity() {
  SblrExecutorAvailabilityRowIdentity identity;
  identity.executor_id = kSblrDmlUpdateRowsExecutorId;
  identity.opcode_code = kSblrDmlUpdateRowsOpcodeCode;
  identity.opcode_version = kSblrDmlUpdateRowsOpcodeVersion;
  identity.operand_descriptor_id = kSblrDmlUpdateRowsOperandDescriptorId;
  identity.result_descriptor_id = kSblrDmlUpdateRowsResultDescriptorId;
  identity.result_descriptor_version =
      kSblrDmlUpdateRowsResultDescriptorVersion;
  return identity;
}

bool DmlUpdateRevalidateExecutorAvailability(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record,
    EngineApiDiagnostic* diagnostic) {
  if (diagnostic == nullptr ||
      record.executor_availability_snapshot.generation == 0 ||
      record.executor_availability_snapshot.snapshot_uuid.empty()) {
    return false;
  }
  SblrExecutorAvailabilitySnapshot current;
  *diagnostic = RevalidateSblrExecutorAvailability(
      context, DmlUpdateRowsExecutorIdentity(),
      record.executor_availability_snapshot, &current);
  return !diagnostic->error &&
         current.generation ==
             record.executor_availability_snapshot.generation;
}

const MgaRelationColumnStorageDescriptor* DmlUpdateFindColumn(
    const MgaRelationStorageDescriptor& relation, std::string_view spelling) {
  const std::string folded = DmlUpdateLowerAscii(spelling);
  const MgaRelationColumnStorageDescriptor* match = nullptr;
  for (const auto& column : relation.columns) {
    if (column.canonical_name_key != spelling &&
        DmlUpdateLowerAscii(column.canonical_name_key) != folded) {
      continue;
    }
    if (match != nullptr) return nullptr;
    match = &column;
  }
  return match;
}

bool DmlUpdateResolveColumnIdentity(
    const EngineRequestContext& context,
    const MgaRelationColumnStorageDescriptor& column,
    DmlUpdateBoundColumnV1* identity,
    EngineApiDiagnostic* diagnostic) {
  if (identity == nullptr || diagnostic == nullptr ||
      column.column_uuid.canonical.empty() ||
      column.column_generation == 0 ||
      column.value_descriptor.descriptor_uuid.canonical.empty()) {
    if (diagnostic != nullptr) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.dml_update_rows.column_identity_absent");
    }
    return false;
  }
  const auto encoded_type_uuid = DmlUpdateDescriptorField(
      column.value_descriptor.encoded_descriptor, "type_uuid");
  const auto encoded_descriptor_uuid = DmlUpdateDescriptorField(
      column.value_descriptor.encoded_descriptor,
      "datatype_descriptor_uuid");
  const auto encoded_descriptor_generation = DmlUpdateDescriptorField(
      column.value_descriptor.encoded_descriptor,
      "datatype_descriptor_generation");
  std::uint64_t descriptor_generation = 0;
  const auto parsed_generation =
      encoded_descriptor_generation.has_value()
          ? std::from_chars(encoded_descriptor_generation->data(),
                            encoded_descriptor_generation->data() +
                                encoded_descriptor_generation->size(),
                            descriptor_generation)
          : std::from_chars_result{};
  if (!encoded_descriptor_uuid.has_value() ||
      !encoded_descriptor_generation.has_value() ||
      parsed_generation.ec != std::errc{} ||
      parsed_generation.ptr != encoded_descriptor_generation->data() +
                                   encoded_descriptor_generation->size() ||
      descriptor_generation == 0) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.dml_update_rows.column_identity_stale",
        column.canonical_name_key);
    return false;
  }
  // The relation column retains its persisted outer descriptor UUID.  The
  // embedded datatype_descriptor_uuid is the canonical datatype-registry
  // identity and is the only valid key for a type/codec lookup.  Conflating
  // these two authorities makes every persisted column look stale.
  const auto lookup = scratchbird::core::datatypes::
      LookupDatatypeTypeCodecIdentityV1(
          context.datatype_catalog_snapshot_uuid.canonical,
          context.datatype_catalog_generation,
          context.datatype_registry_generation,
          *encoded_descriptor_uuid, descriptor_generation);
  if (!lookup.ok || !encoded_type_uuid.has_value() ||
      *encoded_type_uuid != lookup.row.type_uuid ||
      *encoded_descriptor_uuid != lookup.row.descriptor_uuid) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.dml_update_rows.column_identity_stale",
        column.canonical_name_key);
    return false;
  }
  identity->column_uuid = column.column_uuid.canonical;
  identity->column_generation = column.column_generation;
  identity->ordinal = column.ordinal;
  identity->canonical_name_key = column.canonical_name_key;
  identity->datatype_descriptor_uuid = lookup.row.descriptor_uuid;
  identity->datatype_descriptor_generation =
      lookup.row.descriptor_generation;
  identity->type_uuid = lookup.row.type_uuid;
  identity->type_generation = lookup.row.type_generation;
  identity->codec_id = lookup.row.codec_id;
  identity->codec_version = lookup.row.codec_version;
  identity->codec_generation = lookup.row.codec_generation;
  return true;
}

bool DmlUpdateEncodeSignedInteger(
    std::string_view literal,
    const scratchbird::core::datatypes::DatatypeTypeCodecIdentityRowV1& row,
    std::vector<std::uint8_t>* bytes) {
  if (bytes == nullptr ||
      (row.codec_id != "datatype.int32.le.v1" &&
       row.codec_id != "datatype.int64.le.v1")) {
    return false;
  }
  std::int64_t value = 0;
  const auto parsed = std::from_chars(literal.data(),
                                      literal.data() + literal.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != literal.data() + literal.size()) {
    return false;
  }
  if (row.codec_id == "datatype.int32.le.v1" &&
      (value < std::numeric_limits<std::int32_t>::min() ||
       value > std::numeric_limits<std::int32_t>::max())) {
    return false;
  }
  bytes->assign(row.canonical_value_bytes, 0);
  const std::uint64_t bits = static_cast<std::uint64_t>(value);
  for (std::size_t index = 0; index < bytes->size(); ++index) {
    (*bytes)[index] = static_cast<std::uint8_t>(bits >> (index * 8U));
  }
  return bytes->size() == row.canonical_value_bytes;
}

bool DmlUpdateBindLiteral(
    const EngineRequestContext& context,
    const MgaRelationColumnStorageDescriptor& column,
    std::string_view literal,
    EngineTypedValue* value,
    DmlUpdateBoundColumnV1* identity,
    EngineApiDiagnostic* diagnostic) {
  if (value == nullptr || identity == nullptr || diagnostic == nullptr ||
      !DmlUpdateResolveColumnIdentity(context, column, identity, diagnostic)) {
    return false;
  }
  const auto lookup = scratchbird::core::datatypes::
      LookupDatatypeTypeCodecIdentityV1(
          context.datatype_catalog_snapshot_uuid.canonical,
          context.datatype_catalog_generation,
          context.datatype_registry_generation,
          identity->datatype_descriptor_uuid,
          identity->datatype_descriptor_generation);
  std::vector<std::uint8_t> canonical_binary;
  if (!lookup.ok ||
      !DmlUpdateEncodeSignedInteger(literal, lookup.row, &canonical_binary)) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "DATATYPE.DESCRIPTOR_INVALID",
        "sblr.dml_update_rows.literal_codec_refused",
        column.canonical_name_key);
    return false;
  }
  value->descriptor = column.value_descriptor;
  value->encoded_value.assign(literal);
  value->binary_value = std::move(canonical_binary);
  value->setState(EngineValueState::value);
  return true;
}

bool DmlUpdateRevalidateColumn(
    const EngineRequestContext& context,
    const MgaRelationColumnStorageDescriptor& current,
    const DmlUpdateBoundColumnV1& bound) {
  DmlUpdateBoundColumnV1 resolved;
  EngineApiDiagnostic diagnostic;
  return DmlUpdateResolveColumnIdentity(context, current, &resolved,
                                        &diagnostic) &&
         resolved.column_uuid == bound.column_uuid &&
         resolved.column_generation == bound.column_generation &&
         resolved.ordinal == bound.ordinal &&
         resolved.canonical_name_key == bound.canonical_name_key &&
         resolved.datatype_descriptor_uuid ==
             bound.datatype_descriptor_uuid &&
         resolved.datatype_descriptor_generation ==
             bound.datatype_descriptor_generation &&
         resolved.type_uuid == bound.type_uuid &&
         resolved.type_generation == bound.type_generation &&
         resolved.codec_id == bound.codec_id &&
         resolved.codec_version == bound.codec_version &&
         resolved.codec_generation == bound.codec_generation;
}

EngineApiDiagnostic DmlUpdateCarrierDiagnostic(
    const update_wire::TypedUpdateCarrierError& error,
    std::string_view fallback_key) {
  return DmlUpdateDescriptorDiagnostic(
      error.diagnostic_code.empty() ? "SBLR.OPERAND_INVALID"
                                    : error.diagnostic_code,
      std::string(fallback_key),
      error.field.empty() ? error.detail
                          : error.field + ":" + error.detail);
}

bool DmlUpdateBuildAssignmentCarrier(
    DmlUpdateRowsDescriptorRecordV1* record,
    EngineApiDiagnostic* diagnostic) {
  if (record == nullptr || diagnostic == nullptr ||
      record->assignment_columns.empty() ||
      record->assignment_columns.size() !=
          record->prepared_request.assignments.size()) {
    return false;
  }
  auto& carrier = record->canonical_carriers.assignments;
  if (!DmlUpdateIssueTypedIdentity(&carrier.identity.vector_uuid) ||
      !DmlUpdateTypedUuid(record->descriptor_ref.descriptor_uuid,
                          &carrier.identity.owner_descriptor_uuid)) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.assignment_identity_unavailable");
    return false;
  }
  carrier.identity.vector_generation = 1;
  carrier.identity.owner_descriptor_generation =
      record->descriptor_ref.descriptor_generation;
  carrier.records.reserve(record->assignment_columns.size());
  for (std::size_t index = 0; index < record->assignment_columns.size();
       ++index) {
    const auto& column = record->assignment_columns[index];
    const auto& value = record->prepared_request.assignments[index].second;
    update_wire::TypedUpdateAssignmentRecord item;
    item.assignment_ordinal = static_cast<std::uint32_t>(index + 1U);
    if (!DmlUpdateIssueTypedIdentity(&item.assignment_occurrence_uuid) ||
        !DmlUpdateIssueTypedIdentity(&item.target_column_occurrence_uuid) ||
        !DmlUpdateTypedUuid(column.column_uuid, &item.target_column_uuid) ||
        !DmlUpdateTypedUuid(column.datatype_descriptor_uuid,
                            &item.value_descriptor_uuid) ||
        !DmlUpdateTypedUuid(column.type_uuid, &item.value_type_uuid)) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "SBLR.OPERAND_INVALID",
          "sblr.dml_update_rows.assignment_occurrence_unavailable");
      return false;
    }
    item.assignment_occurrence_generation = 1;
    item.target_column_occurrence_generation = 1;
    item.target_column_generation = column.column_generation;
    item.value_descriptor_generation =
        column.datatype_descriptor_generation;
    item.value_type_generation = column.type_generation;
    item.codec_id = column.codec_id;
    item.codec_version = column.codec_version;
    item.codec_generation = column.codec_generation;
    item.value_state = value.isSqlNull()
                           ? update_wire::TypedUpdateValueState::null_value
                           : update_wire::TypedUpdateValueState::value;
    if (!value.isSqlNull()) {
      item.canonical_value.assign(value.binary_value.begin(),
                                  value.binary_value.end());
    }
    carrier.records.push_back(std::move(item));
  }
  update_wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> encoded;
  if (!update_wire::EncodeTypedUpdateAssignmentVector(
          carrier, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateAssignmentVector(
          encoded, &carrier, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.assignment_vector_invalid");
    return false;
  }
  return true;
}

bool DmlUpdateBuildPredicateCarrier(
    DmlUpdateRowsDescriptorRecordV1* record,
    EngineApiDiagnostic* diagnostic) {
  if (record == nullptr || diagnostic == nullptr ||
      record->relation_occurrence_generation == 0 ||
      !record->datatype_operator_binding.ok ||
      record->datatype_operator_binding.datatype_snapshot_uuid !=
          record->datatype_catalog_snapshot_uuid ||
      record->datatype_operator_binding.datatype_catalog_generation !=
          record->datatype_catalog_generation ||
      record->datatype_operator_binding.datatype_registry_generation !=
          record->datatype_registry_generation ||
      record->datatype_operator_binding.boolean_descriptor_generation == 0 ||
      record->datatype_operator_binding.boolean_type_generation == 0 ||
      record->datatype_operator_binding.boolean_codec_id.empty() ||
      record->datatype_operator_binding.boolean_codec_version == 0 ||
      record->datatype_operator_binding.boolean_codec_generation == 0 ||
      record->datatype_operator_binding.builtin_operator_snapshot_uuid.empty() ||
      record->datatype_operator_binding.builtin_operator_registry_generation ==
          0) {
    return false;
  }
  const auto& binding = record->datatype_operator_binding;
  const auto bind_boolean_result = [&binding](
                                       update_wire::TypedUpdatePredicateRecord*
                                           node) {
    if (node == nullptr ||
        !DmlUpdateTypedUuid(binding.boolean_descriptor_uuid,
                            &node->output_descriptor_uuid) ||
        !DmlUpdateTypedUuid(binding.boolean_type_uuid,
                            &node->output_type_uuid)) {
      return false;
    }
    node->output_descriptor_generation =
        binding.boolean_descriptor_generation;
    node->output_type_generation = binding.boolean_type_generation;
    node->output_codec_id = binding.boolean_codec_id;
    node->output_codec_version = binding.boolean_codec_version;
    node->output_codec_generation = binding.boolean_codec_generation;
    return true;
  };
  auto& carrier = record->canonical_carriers.predicate;
  if (!DmlUpdateIssueTypedIdentity(&carrier.identity.vector_uuid) ||
      !DmlUpdateTypedUuid(record->descriptor_ref.descriptor_uuid,
                          &carrier.identity.owner_descriptor_uuid)) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.predicate_identity_unavailable");
    return false;
  }
  carrier.identity.vector_generation = 1;
  carrier.identity.owner_descriptor_generation =
      record->descriptor_ref.descriptor_generation;
  const auto initialize_node = [&](
                                   update_wire::TypedUpdatePredicateRecord* node,
                                   std::uint64_t node_id) {
    node->node_id = node_id;
    node->node_occurrence_generation = 1;
    return DmlUpdateIssueTypedIdentity(&node->node_occurrence_uuid);
  };
  if (!record->predicate_column.has_value()) {
    update_wire::TypedUpdatePredicateRecord node;
    if (!initialize_node(&node, 1) || !bind_boolean_result(&node)) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.dml_update_rows.predicate_boolean_authority_unavailable");
      return false;
    }
    node.node_kind =
        update_wire::TypedUpdatePredicateNodeKind::canonical_boolean_constant;
    node.value_state = update_wire::TypedUpdateValueState::value;
    node.canonical_value = {1};
    carrier.records.push_back(std::move(node));
  } else {
    if (record->prepared_request.update_predicate.bound_values.size() != 1) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "SBLR.OPERAND_INVALID",
          "sblr.dml_update_rows.predicate_value_shape_invalid");
      return false;
    }
    const auto& column = *record->predicate_column;
    const auto& literal =
        record->prepared_request.update_predicate.bound_values.front();
    update_wire::TypedUpdatePredicateRecord column_node;
    update_wire::TypedUpdatePredicateRecord literal_node;
    update_wire::TypedUpdatePredicateRecord compare_node;
    if (!initialize_node(&column_node, 1) ||
        !initialize_node(&literal_node, 2) ||
        !initialize_node(&compare_node, 3) ||
        !DmlUpdateTypedUuid(column.datatype_descriptor_uuid,
                            &column_node.output_descriptor_uuid) ||
        !DmlUpdateTypedUuid(column.type_uuid, &column_node.output_type_uuid) ||
        !DmlUpdateTypedUuid(record->relation_occurrence_uuid,
                            &column_node.referenced_relation_occurrence_uuid) ||
        !DmlUpdateIssueTypedIdentity(
            &column_node.referenced_column_occurrence_uuid) ||
        !DmlUpdateTypedUuid(column.column_uuid,
                            &column_node.referenced_column_uuid)) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "SBLR.OPERAND_INVALID",
          "sblr.dml_update_rows.predicate_occurrence_unavailable");
      return false;
    }
    column_node.node_kind =
        update_wire::TypedUpdatePredicateNodeKind::column_reference;
    column_node.value_state = update_wire::TypedUpdateValueState::absent;
    column_node.output_descriptor_generation =
        column.datatype_descriptor_generation;
    column_node.output_type_generation = column.type_generation;
    column_node.output_codec_id = column.codec_id;
    column_node.output_codec_version = column.codec_version;
    column_node.output_codec_generation = column.codec_generation;
    column_node.referenced_relation_occurrence_generation =
        record->relation_occurrence_generation;
    column_node.referenced_column_occurrence_generation = 1;
    column_node.referenced_column_generation = column.column_generation;

    literal_node.node_kind =
        update_wire::TypedUpdatePredicateNodeKind::typed_literal;
    literal_node.value_state = update_wire::TypedUpdateValueState::value;
    literal_node.output_descriptor_uuid =
        column_node.output_descriptor_uuid;
    literal_node.output_descriptor_generation =
        column_node.output_descriptor_generation;
    literal_node.output_type_uuid = column_node.output_type_uuid;
    literal_node.output_type_generation = column_node.output_type_generation;
    literal_node.output_codec_id = column_node.output_codec_id;
    literal_node.output_codec_version = column_node.output_codec_version;
    literal_node.output_codec_generation = column_node.output_codec_generation;
    literal_node.canonical_value.assign(literal.binary_value.begin(),
                                        literal.binary_value.end());

    if (!bind_boolean_result(&compare_node) ||
        binding.equality_operator_generation == 0 ||
        !DmlUpdateTypedUuid(binding.equality_operator_uuid,
                            &compare_node.operator_uuid)) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DATATYPE.DESCRIPTOR_INVALID",
          "sblr.dml_update_rows.predicate_operator_authority_unavailable");
      return false;
    }
    compare_node.node_kind = update_wire::TypedUpdatePredicateNodeKind::comparison;
    compare_node.value_state = update_wire::TypedUpdateValueState::absent;
    compare_node.left_child_node_id = 1;
    compare_node.right_child_node_id = 2;
    compare_node.operator_generation = binding.equality_operator_generation;
    carrier.records.push_back(std::move(column_node));
    carrier.records.push_back(std::move(literal_node));
    carrier.records.push_back(std::move(compare_node));
  }
  update_wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> encoded;
  if (!update_wire::EncodeTypedUpdatePredicateVector(
          carrier, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdatePredicateVector(
          encoded, &carrier, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.predicate_vector_invalid");
    return false;
  }
  return true;
}

bool DmlUpdateAbandonDurableReservationV1(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record) {
  if (record.durable_operation_identity.validated_durable_handle_uuid.empty()) {
    return true;
  }
  EngineDmlUpdateDurableAuthorityAbandonRequestV1 request;
  request.context = context;
  request.identity = record.durable_operation_identity;
  return AbandonDmlUpdateDurableOperationAuthorityReservationV1(request).ok();
}

bool DmlUpdateBuildExecutionAuthorityCarriers(
    const EngineRequestContext& context,
    DmlUpdateRowsDescriptorRecordV1* record,
    EngineApiDiagnostic* diagnostic) {
  if (record == nullptr || diagnostic == nullptr) return false;
  auto& target_order = record->canonical_carriers.target_order;
  auto& resource = record->canonical_carriers.resource_budget;
  auto& recovery = record->canonical_carriers.recovery_token;
  if (!DmlUpdateIssueTypedIdentity(&target_order.target_order_uuid) ||
      !DmlUpdateTypedUuid(context.statement_receipt_uuid.canonical,
                          &target_order.authenticated_statement_receipt_uuid) ||
      !DmlUpdateTypedUuid(record->relation_occurrence_uuid,
                          &target_order.target_relation_occurrence_uuid) ||
      !DmlUpdateTypedUuid(context.statement_snapshot_uuid.canonical,
                          &target_order.statement_snapshot_uuid) ||
      !DmlUpdateIssueTypedIdentity(&resource.resource_budget_uuid) ||
      !DmlUpdateTypedUuid(context.statement_receipt_uuid.canonical,
                          &resource.authenticated_statement_receipt_uuid) ||
      !DmlUpdateTypedUuid(context.transaction_uuid.canonical,
                          &resource.owning_transaction_uuid) ||
      !DmlUpdateIssueTypedIdentity(&resource.cancellation_token_uuid) ||
      !DmlUpdateIssueTypedIdentity(&resource.grant_receipt_uuid) ||
      !DmlUpdateIssueTypedIdentity(&recovery.recovery_token_uuid) ||
      !DmlUpdateTypedUuid(context.statement_receipt_uuid.canonical,
                          &recovery.authenticated_statement_receipt_uuid) ||
      !DmlUpdateTypedUuid(context.transaction_uuid.canonical,
                          &recovery.owning_transaction_uuid) ||
      !DmlUpdateTypedUuid(record->operation_uuid,
                          &recovery.operation_uuid) ||
      !DmlUpdateTypedUuid(record->descriptor_ref.descriptor_uuid,
                          &recovery.descriptor_uuid) ||
      !DmlUpdateIssueTypedIdentity(
          &recovery.statement_savepoint_profile_uuid)) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "RESOURCE.BUDGET_EXCEEDED",
        "sblr.dml_update_rows.execution_authority_unavailable");
    return false;
  }
  target_order.target_order_generation = 1;
  target_order.target_relation_occurrence_generation =
      record->relation_occurrence_generation;
  target_order.maximum_candidate_rows =
      update_wire::kTypedUpdateMaximumCandidateRows;
  resource.resource_budget_generation = 1;
  resource.cancellation_generation = 1;
  resource.grant_receipt_generation = 1;
  resource.maximum_assignments =
      update_wire::kTypedUpdateMaximumAssignments;
  resource.maximum_predicate_nodes =
      update_wire::kTypedUpdateMaximumPredicateNodes;
  resource.maximum_candidate_rows =
      update_wire::kTypedUpdateMaximumCandidateRows;
  resource.maximum_trigger_depth =
      update_wire::kTypedUpdateMaximumTriggerDepth;
  resource.maximum_effects = update_wire::kTypedUpdateMaximumEffects;
  resource.maximum_total_canonical_value_bytes =
      update_wire::kTypedUpdateMaximumCanonicalValueBytes;
  recovery.recovery_generation = 1;
  recovery.descriptor_generation =
      record->descriptor_ref.descriptor_generation;
  recovery.statement_savepoint_profile_generation = 1;

  update_wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> encoded;
  if (!update_wire::EncodeTypedUpdateTargetOrder(
          target_order, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateTargetOrder(
          encoded, &target_order, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.target_order_invalid");
    return false;
  }
  encoded.clear();
  if (!update_wire::EncodeTypedUpdateResourceBudget(
          resource, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateResourceBudget(
          encoded, &resource, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.resource_budget_invalid");
    return false;
  }
  encoded.clear();
  EngineDmlUpdateDurableAuthorityReservationRequestV1 reserve_request;
  reserve_request.context = context;
  reserve_request.reservation.operation_uuid = record->operation_uuid;
  reserve_request.reservation.operation_generation =
      record->operation_generation;
  reserve_request.reservation.descriptor_uuid =
      record->descriptor_ref.descriptor_uuid;
  reserve_request.reservation.descriptor_generation =
      record->descriptor_ref.descriptor_generation;
  reserve_request.reservation.recovery_token_uuid =
      DmlUpdateUuidText(recovery.recovery_token_uuid);
  reserve_request.reservation.recovery_generation =
      recovery.recovery_generation;
  auto reserved =
      ReserveDmlUpdateDurableOperationAuthorityV1(reserve_request);
  if (!reserved.ok() ||
      reserved.identity.database_uuid != record->database_uuid ||
      reserved.identity.owning_transaction_uuid != record->transaction_uuid ||
      reserved.identity.owning_local_transaction_id !=
          record->local_transaction_id ||
      reserved.identity.authenticated_statement_receipt_uuid !=
          record->statement_receipt_uuid ||
      reserved.identity.operation_uuid != record->operation_uuid ||
      reserved.identity.operation_generation != record->operation_generation ||
      reserved.identity.descriptor_uuid !=
          record->descriptor_ref.descriptor_uuid ||
      reserved.identity.descriptor_generation !=
          record->descriptor_ref.descriptor_generation ||
      reserved.identity.recovery_token_uuid !=
          reserve_request.reservation.recovery_token_uuid ||
      reserved.identity.recovery_generation != recovery.recovery_generation ||
      reserved.identity.validated_durable_handle_generation == 0 ||
      reserved.identity.reserved_statement_barrier_generation == 0 ||
      !DmlUpdateTypedUuid(
          reserved.identity.validated_durable_handle_uuid,
          &recovery.durable_registry_uuid)) {
    *diagnostic = reserved.diagnostic.code.empty()
                      ? DmlUpdateDescriptorDiagnostic(
                            "DML.UPDATE_FAILED",
                            "sblr.dml_update_rows.durable_reservation_invalid")
                      : reserved.diagnostic;
    if (reserved.ok()) {
      EngineDmlUpdateDurableAuthorityAbandonRequestV1 abandon;
      abandon.context = context;
      abandon.identity = reserved.identity;
      (void)AbandonDmlUpdateDurableOperationAuthorityReservationV1(abandon);
    }
    return false;
  }
  record->durable_operation_identity = std::move(reserved.identity);
  recovery.durable_registry_generation =
      record->durable_operation_identity.validated_durable_handle_generation;
  record->publication_barrier_uuid =
      record->durable_operation_identity.reserved_statement_barrier_uuid;
  record->publication_barrier_generation =
      record->durable_operation_identity.reserved_statement_barrier_generation;
  if (record->publication_barrier_uuid.empty() ||
      record->publication_barrier_generation == 0) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.durable_reservation_barrier_invalid");
    (void)DmlUpdateAbandonDurableReservationV1(context, *record);
    return false;
  }
  if (!update_wire::EncodeTypedUpdateRecoveryToken(
          recovery, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateRecoveryToken(
          encoded, &recovery, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.recovery_token_invalid");
    (void)DmlUpdateAbandonDurableReservationV1(context, *record);
    return false;
  }
  return true;
}

bool DmlUpdateBuildFrozenAuthorityCarriers(
    const EngineDmlUpdateImmutableAuthoritySnapshotV1& snapshot,
    DmlUpdateRowsDescriptorRecordV1* record,
    EngineApiDiagnostic* diagnostic) {
  if (record == nullptr || diagnostic == nullptr) return false;
  update_wire::TypedUpdateUuid owner_uuid{};
  if (!DmlUpdateTypedUuid(record->descriptor_ref.descriptor_uuid,
                          &owner_uuid)) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.frozen_owner_invalid");
    return false;
  }
  auto initialize_identity = [&](std::string_view set_uuid,
                                 std::uint64_t set_generation,
                                 update_wire::TypedUpdateVectorIdentity* out) {
    return out != nullptr && set_generation != 0 &&
           DmlUpdateTypedUuid(set_uuid, &out->vector_uuid) &&
           ((out->vector_generation = set_generation), true) &&
           ((out->owner_descriptor_uuid = owner_uuid), true) &&
           ((out->owner_descriptor_generation =
                 record->descriptor_ref.descriptor_generation),
            true);
  };
  auto& policies = record->canonical_carriers.row_policies;
  auto& constraints = record->canonical_carriers.constraints;
  auto& triggers = record->canonical_carriers.triggers;
  if (!initialize_identity(snapshot.row_policy_set.set_uuid,
                           snapshot.row_policy_set.set_generation,
                           &policies.identity) ||
      !initialize_identity(snapshot.constraint_set.set_uuid,
                           snapshot.constraint_set.set_generation,
                           &constraints.identity) ||
      !initialize_identity(snapshot.trigger_set.set_uuid,
                           snapshot.trigger_set.set_generation,
                           &triggers.identity)) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.frozen_set_identity_invalid");
    return false;
  }
  policies.records.reserve(snapshot.row_policy_set.records.size());
  for (const auto& source : snapshot.row_policy_set.records) {
    update_wire::TypedUpdateRowPolicyRecord item;
    item.policy_ordinal = source.policy_ordinal;
    item.phase = static_cast<std::uint8_t>(source.phase);
    if (!DmlUpdateTypedUuid(source.effective_policy_uuid,
                            &item.effective_policy_uuid) ||
        !DmlUpdateTypedUuid(source.expression_uuid, &item.expression_uuid) ||
        !DmlUpdateTypedUuid(source.security_snapshot_uuid,
                            &item.security_snapshot_uuid)) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "SECURITY.ACCESS_DENIED",
          "sblr.dml_update_rows.row_policy_identity_invalid");
      return false;
    }
    item.effective_policy_generation =
        source.effective_policy_generation;
    item.expression_generation = source.expression_generation;
    item.expression_evidence_sha256 = source.expression_evidence_sha256;
    item.security_generation = source.security_generation;
    item.source_policy_catalog_vector_sha256 =
        source.source_policy_catalog_vector_sha256;
    policies.records.push_back(std::move(item));
  }
  constraints.records.reserve(snapshot.constraint_set.records.size());
  for (const auto& source : snapshot.constraint_set.records) {
    update_wire::TypedUpdateConstraintRecord item;
    item.constraint_ordinal = source.constraint_ordinal;
    item.constraint_class = static_cast<std::uint8_t>(source.constraint_class);
    item.timing = static_cast<std::uint8_t>(source.timing);
    item.reservation_mode = static_cast<std::uint8_t>(source.reservation_mode);
    if (!DmlUpdateTypedUuid(source.constraint_uuid, &item.constraint_uuid) ||
        !DmlUpdateTypedUuid(source.reservation_profile_uuid,
                            &item.reservation_profile_uuid) ||
        (source.expression_generation != 0 &&
         !DmlUpdateTypedUuid(source.expression_uuid,
                             &item.expression_uuid))) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.constraint_identity_invalid");
      return false;
    }
    item.constraint_generation = source.constraint_generation;
    item.expression_generation = source.expression_generation;
    item.reservation_profile_generation =
        source.reservation_profile_generation;
    item.dependency_set_sha256 = source.dependency_set_sha256;
    constraints.records.push_back(std::move(item));
  }
  triggers.records.reserve(snapshot.trigger_set.records.size());
  for (const auto& source : snapshot.trigger_set.records) {
    update_wire::TypedUpdateTriggerRecord item;
    item.trigger_ordinal = source.trigger_ordinal;
    item.timing = static_cast<std::uint8_t>(source.timing);
    item.security_mode = static_cast<std::uint8_t>(source.security_mode);
    if (!DmlUpdateTypedUuid(source.trigger_uuid, &item.trigger_uuid) ||
        !DmlUpdateTypedUuid(source.body_sblr_uuid, &item.body_sblr_uuid) ||
        !DmlUpdateTypedUuid(source.execution_security_context_uuid,
                            &item.execution_security_context_uuid) ||
        !DmlUpdateTypedUuid(source.recursion_profile_uuid,
                            &item.recursion_profile_uuid)) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "SBLR.OPERATION_UNSUPPORTED",
          "sblr.dml_update_rows.trigger_identity_invalid");
      return false;
    }
    item.trigger_generation = source.trigger_generation;
    item.body_sblr_generation = source.body_sblr_generation;
    item.execution_security_generation =
        source.execution_security_generation;
    item.recursion_profile_generation = source.recursion_profile_generation;
    item.maximum_depth = source.maximum_depth;
    item.dependency_set_sha256 = source.dependency_set_sha256;
    triggers.records.push_back(std::move(item));
  }
  update_wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> encoded;
  if (!update_wire::EncodeTypedUpdateRowPolicyVector(
          policies, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateRowPolicyVector(
          encoded, &policies, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.row_policy_vector_invalid");
    return false;
  }
  encoded.clear();
  if (!update_wire::EncodeTypedUpdateConstraintVector(
          constraints, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateConstraintVector(
          encoded, &constraints, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.constraint_vector_invalid");
    return false;
  }
  encoded.clear();
  if (!update_wire::EncodeTypedUpdateTriggerVector(
          triggers, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateTriggerVector(
          encoded, &triggers, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.trigger_vector_invalid");
    return false;
  }
  const auto policy_records_match = [&] {
    if (policies.records.size() != snapshot.row_policy_set.records.size()) {
      return false;
    }
    for (std::size_t index = 0; index < policies.records.size(); ++index) {
      if (policies.records[index].record_evidence_sha256 !=
          snapshot.row_policy_set.records[index].record_evidence_sha256) {
        return false;
      }
    }
    return true;
  };
  const auto constraint_records_match = [&] {
    if (constraints.records.size() != snapshot.constraint_set.records.size()) {
      return false;
    }
    for (std::size_t index = 0; index < constraints.records.size(); ++index) {
      if (constraints.records[index].record_evidence_sha256 !=
          snapshot.constraint_set.records[index].record_evidence_sha256) {
        return false;
      }
    }
    return true;
  };
  const auto trigger_records_match = [&] {
    if (triggers.records.size() != snapshot.trigger_set.records.size()) {
      return false;
    }
    for (std::size_t index = 0; index < triggers.records.size(); ++index) {
      if (triggers.records[index].record_evidence_sha256 !=
          snapshot.trigger_set.records[index].record_evidence_sha256) {
        return false;
      }
    }
    return true;
  };
  if (policies.identity.vector_sha256 !=
          snapshot.row_policy_set.vector_sha256 ||
      constraints.identity.vector_sha256 !=
          snapshot.constraint_set.vector_sha256 ||
      triggers.identity.vector_sha256 != snapshot.trigger_set.vector_sha256 ||
      !policy_records_match() || !constraint_records_match() ||
      !trigger_records_match()) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.frozen_provider_codec_evidence_mismatch");
    return false;
  }
  return true;
}

bool DmlUpdateBuildDescriptorCarrier(
    DmlUpdateRowsDescriptorRecordV1* record,
    EngineApiDiagnostic* diagnostic) {
  if (record == nullptr || diagnostic == nullptr) return false;
  auto& value = record->canonical_carriers.descriptor;
  const auto& assignment = record->canonical_carriers.assignments;
  const auto& predicate = record->canonical_carriers.predicate;
  const auto& policies = record->canonical_carriers.row_policies;
  const auto& constraints = record->canonical_carriers.constraints;
  const auto& triggers = record->canonical_carriers.triggers;
  const auto& target_order = record->canonical_carriers.target_order;
  const auto& resource = record->canonical_carriers.resource_budget;
  const auto& recovery = record->canonical_carriers.recovery_token;
  if (!DmlUpdateTypedUuid(record->descriptor_ref.descriptor_uuid,
                          &value.descriptor_uuid) ||
      !DmlUpdateTypedUuid(record->statement_receipt_uuid,
                          &value.authenticated_statement_receipt_uuid) ||
      !DmlUpdateTypedUuid(record->operation_uuid, &value.operation_uuid) ||
      !DmlUpdateTypedUuid(record->transaction_uuid,
                          &value.owning_transaction_uuid) ||
      !DmlUpdateTypedUuid(record->statement_snapshot_uuid,
                          &value.statement_snapshot_uuid) ||
      !DmlUpdateTypedUuid(record->metadata_snapshot_uuid,
                          &value.catalog_snapshot_uuid) ||
      !DmlUpdateTypedUuid(record->security_context_uuid,
                          &value.security_context_uuid) ||
      !DmlUpdateTypedUuid(record->security_snapshot_uuid,
                          &value.security_snapshot_uuid) ||
      !DmlUpdateTypedUuid(record->relation_uuid,
                          &value.target_relation_uuid) ||
      !DmlUpdateTypedUuid(record->relation_occurrence_uuid,
                          &value.target_relation_occurrence_uuid) ||
      !record->datatype_operator_binding.ok ||
      !DmlUpdateTypedUuid(
          record->datatype_operator_binding.builtin_operator_snapshot_uuid,
          &value.builtin_operator_snapshot_uuid) ||
      record->datatype_operator_binding.builtin_operator_registry_generation ==
          0) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.descriptor_authority_identity_invalid");
    return false;
  }
  value.descriptor_generation = record->descriptor_ref.descriptor_generation;
  value.structural_occurrence_id = record->structural_occurrence_id;
  value.operation_generation = record->operation_generation;
  value.owning_local_transaction_id = record->local_transaction_id;
  value.catalog_generation = record->catalog_generation_id;
  value.datatype_registry_generation =
      record->datatype_registry_generation;
  value.security_generation = record->security_generation;
  value.target_relation_generation = record->relation_generation;
  value.target_relation_occurrence_generation =
      record->relation_occurrence_generation;
  value.assignment_vector_uuid = assignment.identity.vector_uuid;
  value.assignment_vector_generation = assignment.identity.vector_generation;
  value.assignment_count = static_cast<std::uint32_t>(
      assignment.records.size());
  value.assignment_vector_sha256 = assignment.identity.vector_sha256;
  value.predicate_expression_uuid = predicate.identity.vector_uuid;
  value.predicate_expression_generation =
      predicate.identity.vector_generation;
  value.predicate_root_node_id = predicate.records.size();
  value.predicate_node_count = static_cast<std::uint32_t>(
      predicate.records.size());
  value.predicate_vector_sha256 = predicate.identity.vector_sha256;
  value.row_policy_set_uuid = policies.identity.vector_uuid;
  value.row_policy_set_generation = policies.identity.vector_generation;
  value.row_policy_count = static_cast<std::uint32_t>(
      policies.records.size());
  value.row_policy_set_sha256 = policies.identity.vector_sha256;
  value.constraint_set_uuid = constraints.identity.vector_uuid;
  value.constraint_set_generation = constraints.identity.vector_generation;
  value.constraint_count = static_cast<std::uint32_t>(
      constraints.records.size());
  value.ordered_constraint_set_sha256 = constraints.identity.vector_sha256;
  value.trigger_set_uuid = triggers.identity.vector_uuid;
  value.trigger_set_generation = triggers.identity.vector_generation;
  value.trigger_count = static_cast<std::uint32_t>(triggers.records.size());
  value.ordered_trigger_set_sha256 = triggers.identity.vector_sha256;
  value.deterministic_target_order_uuid = target_order.target_order_uuid;
  value.deterministic_target_order_generation =
      target_order.target_order_generation;
  value.resource_budget_uuid = resource.resource_budget_uuid;
  value.resource_budget_generation = resource.resource_budget_generation;
  value.recovery_token_uuid = recovery.recovery_token_uuid;
  value.recovery_generation = recovery.recovery_generation;
  value.executor_availability_generation =
      record->executor_availability_snapshot.generation;
  value.builtin_operator_registry_generation =
      record->datatype_operator_binding.builtin_operator_registry_generation;

  update_wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> encoded;
  if (!update_wire::EncodeTypedUpdateDescriptor(value, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateDescriptor(
          encoded, &value, &error) ||
      !update_wire::ValidateTypedUpdateCarrierSet(
          record->canonical_carriers, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.descriptor_carrier_invalid");
    return false;
  }
  return true;
}

bool DmlUpdateCaptureCanonicalProviderAuthorities(
    const EngineRequestContext& context,
    DmlUpdateRowsDescriptorRecordV1* record,
    EngineApiDiagnostic* diagnostic) {
  if (record == nullptr || diagnostic == nullptr ||
      !record->policy_catalog_authority.ok ||
      record->canonical_carriers.descriptor.exact_bytes.empty() ||
      record->canonical_carriers.assignments.exact_bytes.empty() ||
      record->canonical_carriers.predicate.exact_bytes.empty() ||
      record->canonical_carriers.row_policies.exact_bytes.empty() ||
      record->policy_catalog_authority
          .exact_source_policy_vector_dusv.empty()) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.provider_authority_input_invalid");
    return false;
  }

  EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1 datatype_request;
  datatype_request.context = context;
  datatype_request.authenticated_statement_receipt_uuid =
      record->statement_receipt_uuid;
  datatype_request.exact_descriptor_dudc =
      record->canonical_carriers.descriptor.exact_bytes;
  datatype_request.exact_assignment_vector_duav =
      record->canonical_carriers.assignments.exact_bytes;
  datatype_request.exact_predicate_vector_duev =
      record->canonical_carriers.predicate.exact_bytes;
  auto datatype_authority =
      CaptureDmlUpdateDatatypeOperatorAuthorityV1(datatype_request);
  if (!datatype_authority.ok ||
      datatype_authority.exact_datatype_authority_dudv.empty() ||
      datatype_authority.exact_builtin_operator_authority_duov.empty() ||
      datatype_authority.datatypes.exact_bytes !=
          datatype_authority.exact_datatype_authority_dudv ||
      datatype_authority.operators.exact_bytes !=
          datatype_authority.exact_builtin_operator_authority_duov) {
    *diagnostic = datatype_authority.diagnostic.code.empty()
                      ? DmlUpdateDescriptorDiagnostic(
                            "DATATYPE.DESCRIPTOR_INVALID",
                            "sblr.dml_update_rows.datatype_operator_capture_failed")
                      : datatype_authority.diagnostic;
    return false;
  }

  EngineDmlUpdateSecuritySnapshotProofRequestV1 security_request;
  security_request.context = context;
  security_request.captured = record->policy_catalog_authority;
  security_request.exact_descriptor_dudc =
      record->canonical_carriers.descriptor.exact_bytes;
  security_request.exact_row_policy_vector_dupv =
      record->canonical_carriers.row_policies.exact_bytes;
  auto security_proof =
      BuildDmlUpdateSecuritySnapshotProofV1(security_request);
  if (!security_proof.ok ||
      security_proof.exact_security_snapshot_proof_dusp.empty() ||
      security_proof.proof.exact_bytes !=
          security_proof.exact_security_snapshot_proof_dusp ||
      record->policy_catalog_authority.source_policy_vector.exact_bytes !=
          record->policy_catalog_authority
              .exact_source_policy_vector_dusv) {
    *diagnostic = security_proof.diagnostic.code.empty()
                      ? DmlUpdateDescriptorDiagnostic(
                            "DML.UPDATE_FAILED",
                            "sblr.dml_update_rows.security_snapshot_capture_failed")
                      : security_proof.diagnostic;
    return false;
  }

  record->canonical_source_policies =
      record->policy_catalog_authority.source_policy_vector;
  record->canonical_security_snapshot = std::move(security_proof.proof);
  record->canonical_datatype_authority = datatype_authority.datatypes;
  record->canonical_operator_authority = datatype_authority.operators;
  record->datatype_operator_authority = std::move(datatype_authority);
  return true;
}

EngineDmlUpdateImmutableAuthorityFreezeRequestV1
DmlUpdateCurrentImmutableAuthorityRequest(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record) {
  EngineDmlUpdateImmutableAuthorityFreezeRequestV1 request;
  request.context = context;
  request.authenticated_statement_receipt_uuid = record.statement_receipt_uuid;
  request.structural_occurrence_id = record.structural_occurrence_id;
  request.relation_occurrence.relation_uuid = record.relation_uuid;
  request.relation_occurrence.relation_generation = record.relation_generation;
  request.relation_occurrence.relation_occurrence_uuid =
      record.relation_occurrence_uuid;
  request.relation_occurrence.relation_occurrence_generation =
      record.relation_occurrence_generation;
  request.catalog_snapshot_uuid = record.metadata_snapshot_uuid;
  request.catalog_generation = record.catalog_generation_id;
  if (record.policy_catalog_authority.ok) {
    request.security_policy_snapshot_authority =
        record.policy_catalog_authority.security_policy_snapshot;
    request.row_policies =
        record.policy_catalog_authority.immutable_policy_sources;
  }
  return request;
}

EngineDmlUpdateStatementMgaAuthorityOpenRequestV1
DmlUpdateCurrentStatementMgaAuthorityRequest(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record) {
  EngineDmlUpdateStatementMgaAuthorityOpenRequestV1 request;
  request.context = context;
  request.authenticated_statement_receipt_uuid =
      record.statement_receipt_uuid;
  request.operation_uuid = record.operation_uuid;
  request.descriptor_uuid = record.descriptor_ref.descriptor_uuid;
  request.descriptor_generation =
      record.descriptor_ref.descriptor_generation;
  request.recovery_token_uuid = DmlUpdateUuidText(
      record.canonical_carriers.recovery_token.recovery_token_uuid);
  request.recovery_generation =
      record.canonical_carriers.recovery_token.recovery_generation;
  request.reserved_publication_barrier_uuid =
      record.durable_operation_identity.reserved_statement_barrier_uuid;
  request.reserved_publication_barrier_generation =
      record.durable_operation_identity.reserved_statement_barrier_generation;
  return request;
}

bool DmlUpdateApplyStatementMgaAuthority(
    const MgaDmlUpdateStatementSavepointAuthorityV1& authority,
    DmlUpdateRowsDescriptorRecordV1* record) {
  if (record == nullptr || authority.savepoint_uuid.empty() ||
      authority.savepoint_generation != 1 ||
      authority.publication_barrier_uuid.empty() ||
      authority.publication_barrier_generation != 1 ||
      authority.publication_barrier_uuid !=
          record->durable_operation_identity.reserved_statement_barrier_uuid ||
      authority.publication_barrier_generation !=
          record->durable_operation_identity
              .reserved_statement_barrier_generation) {
    return false;
  }
  record->statement_mga_authority = authority;
  record->statement_savepoint_uuid = authority.savepoint_uuid;
  record->statement_savepoint_generation = authority.savepoint_generation;
  record->publication_barrier_uuid = authority.publication_barrier_uuid;
  record->publication_barrier_generation =
      authority.publication_barrier_generation;
  return true;
}

bool DmlUpdateApplyReleasedStatementMgaAuthorityNoAllocV1(
    const MgaDmlUpdateStatementSavepointAuthorityV1& authority,
    DmlUpdateRowsDescriptorRecordV1* record) noexcept {
  if (record == nullptr ||
      authority.lifecycle !=
          MgaDmlUpdateStatementSavepointLifecycleV1::released ||
      !authority.publication_barrier_present ||
      authority.savepoint_generation != 1 ||
      authority.publication_barrier_generation != 1 ||
      record->statement_savepoint_uuid != authority.savepoint_uuid ||
      record->statement_savepoint_generation !=
          authority.savepoint_generation ||
      record->publication_barrier_uuid !=
          authority.publication_barrier_uuid ||
      record->publication_barrier_generation !=
          authority.publication_barrier_generation ||
      record->statement_mga_authority.binding != authority.binding ||
      record->statement_mga_authority.savepoint_uuid !=
          authority.savepoint_uuid ||
      record->statement_mga_authority.publication_barrier_uuid !=
          authority.publication_barrier_uuid) {
    return false;
  }
  // Every string and generation was frozen while the savepoint was active.
  // Only fixed-size lifecycle/presence evidence changes after release.
  record->statement_mga_authority.lifecycle = authority.lifecycle;
  record->statement_mga_authority.publication_barrier_present = true;
  record->statement_mga_authority.durable_presence_sha256 =
      authority.durable_presence_sha256;
  return true;
}

bool DmlUpdateRevalidateCanonicalAuthority(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record,
    EngineApiDiagnostic* diagnostic) {
  if (diagnostic == nullptr) return false;
  update_wire::TypedUpdateCarrierError carrier_error;
  if (!update_wire::ValidateTypedUpdateCarrierSet(
          record.canonical_carriers, &carrier_error) ||
      !update_wire::ValidateTypedUpdateDatatypeOperatorAuthority(
          record.canonical_carriers.descriptor,
          record.canonical_carriers.assignments,
          record.canonical_carriers.predicate,
          record.canonical_datatype_authority,
          record.canonical_operator_authority, &carrier_error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        carrier_error, "sblr.dml_update_rows.carrier_set_stale");
    return false;
  }
  const auto& descriptor = record.canonical_carriers.descriptor;
  update_wire::TypedUpdateUuid receipt_uuid{};
  update_wire::TypedUpdateUuid transaction_uuid{};
  update_wire::TypedUpdateUuid statement_snapshot_uuid{};
  update_wire::TypedUpdateUuid catalog_snapshot_uuid{};
  update_wire::TypedUpdateUuid relation_uuid{};
  update_wire::TypedUpdateUuid security_context_uuid{};
  update_wire::TypedUpdateUuid security_snapshot_uuid{};
  if (!DmlUpdateTypedUuid(context.statement_receipt_uuid.canonical,
                          &receipt_uuid) ||
      !DmlUpdateTypedUuid(context.transaction_uuid.canonical,
                          &transaction_uuid) ||
      !DmlUpdateTypedUuid(context.statement_snapshot_uuid.canonical,
                          &statement_snapshot_uuid) ||
      !DmlUpdateTypedUuid(context.statement_metadata_snapshot_uuid.canonical,
                          &catalog_snapshot_uuid) ||
      !DmlUpdateTypedUuid(record.relation_uuid, &relation_uuid) ||
      !DmlUpdateTypedUuid(record.security_context_uuid,
                          &security_context_uuid) ||
      !DmlUpdateTypedUuid(record.security_snapshot_uuid,
                          &security_snapshot_uuid) ||
      descriptor.authenticated_statement_receipt_uuid != receipt_uuid ||
      descriptor.owning_transaction_uuid != transaction_uuid ||
      descriptor.owning_local_transaction_id != context.local_transaction_id ||
      descriptor.statement_snapshot_uuid != statement_snapshot_uuid ||
      descriptor.catalog_snapshot_uuid != catalog_snapshot_uuid ||
      descriptor.catalog_generation != context.catalog_generation_id ||
      descriptor.datatype_registry_generation !=
          context.datatype_registry_generation ||
      !context.authorization_context.present ||
      context.authorization_context.authority_uuid.canonical !=
          record.security_context_uuid ||
      context.authorization_context.security_context_generation !=
          record.security_context_generation ||
      descriptor.security_context_uuid != security_context_uuid ||
      descriptor.security_snapshot_uuid != security_snapshot_uuid ||
      descriptor.security_generation != record.security_generation ||
      descriptor.target_relation_uuid != relation_uuid ||
      descriptor.target_relation_generation != record.relation_generation ||
      !record.datatype_operator_binding.ok ||
      DmlUpdateUuidText(descriptor.builtin_operator_snapshot_uuid) !=
          record.datatype_operator_binding.builtin_operator_snapshot_uuid ||
      descriptor.builtin_operator_registry_generation !=
          record.datatype_operator_binding
              .builtin_operator_registry_generation) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.descriptor_live_authority_stale");
    return false;
  }

  const auto datatype_revalidation =
      RevalidateDmlUpdateDatatypeOperatorAuthorityV1(
          context, record.datatype_operator_authority);
  if (datatype_revalidation.error) {
    *diagnostic = datatype_revalidation;
    return false;
  }

  EngineDmlUpdateImmutableAuthorityRevalidateRequestV1 immutable_request;
  immutable_request.current =
      DmlUpdateCurrentImmutableAuthorityRequest(context, record);
  immutable_request.admitted = record.immutable_authority_snapshot;
  const auto immutable =
      RevalidateDmlUpdateImmutableAuthorityV1(immutable_request);
  if (!immutable.ok) {
    *diagnostic = immutable.diagnostic;
    return false;
  }
  return true;
}

std::optional<update_wire::TypedUpdateJournalState>
DmlUpdateJournalStateForRecord(
    const DmlUpdateRowsDescriptorRecordV1& record) {
  switch (record.lifecycle) {
    case DmlUpdateDescriptorLifecycleV1::kLive:
      return update_wire::TypedUpdateJournalState::bound;
    case DmlUpdateDescriptorLifecycleV1::kExecuting:
      return update_wire::TypedUpdateJournalState::intent;
    case DmlUpdateDescriptorLifecycleV1::kPrepared:
      return update_wire::TypedUpdateJournalState::prepared;
    case DmlUpdateDescriptorLifecycleV1::kCompleted:
      return update_wire::TypedUpdateJournalState::published;
    case DmlUpdateDescriptorLifecycleV1::kFailed:
      return update_wire::TypedUpdateJournalState::aborted;
  }
  return std::nullopt;
}

bool DmlUpdateBuildJournalRecordV1(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record,
    std::vector<std::uint8_t>* encoded,
    update_wire::TypedUpdateJournalState* next_state,
    update_wire::TypedUpdateHash* next_evidence) {
  if (encoded == nullptr || next_state == nullptr || next_evidence == nullptr) {
    return false;
  }
  const auto state = DmlUpdateJournalStateForRecord(record);
  if (!state.has_value()) return false;
  update_wire::TypedUpdateJournalRecord journal;
  journal.lifecycle_state = *state;
  journal.journal_sequence = record.journal_sequence + 1U;
  journal.descriptor = record.canonical_carriers.descriptor;
  journal.authenticated_statement_receipt_uuid =
      journal.descriptor.authenticated_statement_receipt_uuid;
  journal.owning_transaction_uuid =
      journal.descriptor.owning_transaction_uuid;
  journal.owning_local_transaction_id =
      journal.descriptor.owning_local_transaction_id;
  journal.operation_uuid = journal.descriptor.operation_uuid;
  journal.recovery_token_uuid = journal.descriptor.recovery_token_uuid;
  journal.recovery_generation = journal.descriptor.recovery_generation;
  if (!DmlUpdateTypedUuid(context.database_uuid.canonical,
                          &journal.database_uuid)) {
    return false;
  }
  if (!record.statement_savepoint_uuid.empty() &&
      (!DmlUpdateTypedUuid(record.statement_savepoint_uuid,
                           &journal.statement_savepoint_uuid) ||
       record.statement_savepoint_generation == 0)) {
    return false;
  }
  journal.statement_savepoint_generation =
      record.statement_savepoint_generation;
  journal.prior_record_sha256 = record.latest_journal_evidence_sha256;
  if (*state == update_wire::TypedUpdateJournalState::prepared ||
      *state == update_wire::TypedUpdateJournalState::published) {
    update_wire::TypedUpdateResultCarrier result;
    update_wire::TypedUpdateCarrierError error;
    if (!update_wire::DecodeAndValidateTypedUpdateResult(
            record.canonical_result_bytes, &result, &error)) {
      return false;
    }
    journal.prior_result = std::move(result);
  }

  update_wire::TypedUpdateJournalChainContext chain;
  chain.first_record = record.journal_sequence == 0;
  chain.prior_sequence = record.journal_sequence;
  chain.prior_state = record.latest_journal_state;
  chain.prior_record_evidence_sha256 =
      record.latest_journal_evidence_sha256;
  if (!record.statement_savepoint_uuid.empty() &&
      !DmlUpdateTypedUuid(record.statement_savepoint_uuid,
                          &chain.prior_savepoint_uuid)) {
    return false;
  }
  chain.prior_savepoint_generation = record.statement_savepoint_generation;
  if (!chain.first_record) {
    std::vector<std::uint8_t> descriptor_bytes;
    update_wire::TypedUpdateCarrierError error;
    if (!update_wire::EncodeTypedUpdateDescriptor(
            record.canonical_carriers.descriptor, &descriptor_bytes, &error) ||
        descriptor_bytes.size() !=
            update_wire::kTypedUpdateDescriptorBytes) {
      return false;
    }
    chain.require_same_descriptor = true;
    std::copy(descriptor_bytes.begin(), descriptor_bytes.end(),
              chain.expected_descriptor_bytes.begin());
    if (chain.prior_state ==
            update_wire::TypedUpdateJournalState::prepared &&
        *state == update_wire::TypedUpdateJournalState::published) {
      if (record.canonical_result_bytes.size() !=
          update_wire::kTypedUpdateResultBytes) {
        return false;
      }
      std::array<std::uint8_t, update_wire::kTypedUpdateResultBytes>
          expected_result{};
      std::copy(record.canonical_result_bytes.begin(),
                record.canonical_result_bytes.end(),
                expected_result.begin());
      chain.expected_prepared_result_bytes = expected_result;
    }
  }
  update_wire::TypedUpdateCarrierError error;
  if (!update_wire::EncodeTypedUpdateJournalRecord(
          journal, chain, encoded, &error) ||
      !update_wire::ExtractTypedUpdateJournalRecordEvidence(
          *encoded, next_evidence, &error)) {
    return false;
  }
  *next_state = *state;
  return true;
}

struct DmlUpdatePreparedJournalAppendV1 {
  std::vector<std::uint8_t> exact_dujr_bytes;
  std::uint64_t expected_prior_sequence = 0;
  update_wire::TypedUpdateJournalState expected_prior_state =
      update_wire::TypedUpdateJournalState::bound;
  update_wire::TypedUpdateHash expected_prior_evidence_sha256{};
  update_wire::TypedUpdateJournalState next_state =
      update_wire::TypedUpdateJournalState::bound;
  update_wire::TypedUpdateHash next_evidence_sha256{};
};

// Provider-owned prebuilt successor.  Preparing this value may allocate,
// encode and hash while rollback remains legal.  Commit consumes only the
// already-built MGA frame after the statement publication barrier.
struct DmlUpdatePreparedDurableSuccessorV1 {
  std::uint64_t expected_prior_sequence = 0;
  update_wire::TypedUpdateJournalState expected_prior_state =
      update_wire::TypedUpdateJournalState::bound;
  update_wire::TypedUpdateHash expected_prior_evidence_sha256{};
  update_wire::TypedUpdateJournalState next_state =
      update_wire::TypedUpdateJournalState::bound;
  update_wire::TypedUpdateHash next_evidence_sha256{};
  MgaDmlUpdateDurablePreparedSuccessorV1 provider_prepared;
};

std::optional<MgaDmlUpdateDurableJournalStateV1>
DmlUpdateDurableJournalStateV1(update_wire::TypedUpdateJournalState state) {
  switch (state) {
    case update_wire::TypedUpdateJournalState::bound:
      return MgaDmlUpdateDurableJournalStateV1::bound;
    case update_wire::TypedUpdateJournalState::intent:
      return MgaDmlUpdateDurableJournalStateV1::intent;
    case update_wire::TypedUpdateJournalState::prepared:
      return MgaDmlUpdateDurableJournalStateV1::prepared;
    case update_wire::TypedUpdateJournalState::published:
      return MgaDmlUpdateDurableJournalStateV1::published;
    case update_wire::TypedUpdateJournalState::aborted:
      return MgaDmlUpdateDurableJournalStateV1::aborted;
  }
  return std::nullopt;
}

std::optional<update_wire::TypedUpdateJournalState>
DmlUpdateWireJournalStateV1(MgaDmlUpdateDurableJournalStateV1 state) {
  switch (state) {
    case MgaDmlUpdateDurableJournalStateV1::bound:
      return update_wire::TypedUpdateJournalState::bound;
    case MgaDmlUpdateDurableJournalStateV1::intent:
      return update_wire::TypedUpdateJournalState::intent;
    case MgaDmlUpdateDurableJournalStateV1::prepared:
      return update_wire::TypedUpdateJournalState::prepared;
    case MgaDmlUpdateDurableJournalStateV1::published:
      return update_wire::TypedUpdateJournalState::published;
    case MgaDmlUpdateDurableJournalStateV1::aborted:
      return update_wire::TypedUpdateJournalState::aborted;
  }
  return std::nullopt;
}

struct DmlUpdateDecodedDurableRecoveryV1 {
  update_wire::TypedUpdateCarrierSet carriers;
  update_wire::TypedUpdateSecurityPolicySourceVector source_policies;
  update_wire::TypedUpdateSecuritySnapshotProof security_snapshot;
  update_wire::TypedUpdateDatatypeAuthorityVector datatype_authority;
  update_wire::TypedUpdateBuiltinOperatorAuthorityVector operator_authority;
  update_wire::TypedUpdateMgaRecoveryObservation recovery_observation;
  std::vector<update_wire::TypedUpdateJournalRecord> journal;
  std::optional<update_wire::TypedUpdateJournalRecord> staged_successor;
  update_wire::TypedUpdateRecoveryDecision decision =
      update_wire::TypedUpdateRecoveryDecision::quarantine_update_failed;
};

bool DmlUpdateDecodeDurableRecoveryV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    std::uint64_t structural_occurrence_id,
    const MgaDmlUpdateDurableOperationRecoveryResultV1& recovered,
    DmlUpdateDecodedDurableRecoveryV1* decoded,
    EngineApiDiagnostic* diagnostic) {
  if (decoded == nullptr || diagnostic == nullptr || !recovered.ok() ||
      recovered.journal.empty() || structural_occurrence_id == 0 ||
      descriptor_ref.descriptor_generation == 0) {
    return false;
  }
  const auto fail = [&](const update_wire::TypedUpdateCarrierError& error,
                        std::string_view key) {
    *diagnostic = DmlUpdateCarrierDiagnostic(error, key);
    return false;
  };
  update_wire::TypedUpdateCarrierError error;
  auto& snapshot = recovered.authority_snapshot;
  if (!update_wire::DecodeAndValidateTypedUpdateDescriptor(
          snapshot.descriptor_dudc, &decoded->carriers.descriptor, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateAssignmentVector(
          snapshot.assignment_vector_duav, &decoded->carriers.assignments,
          &error) ||
      !update_wire::DecodeAndValidateTypedUpdatePredicateVector(
          snapshot.predicate_vector_duev, &decoded->carriers.predicate,
          &error) ||
      !update_wire::DecodeAndValidateTypedUpdateRowPolicyVector(
          snapshot.row_policy_vector_dupv, &decoded->carriers.row_policies,
          &error) ||
      !update_wire::DecodeAndValidateTypedUpdateConstraintVector(
          snapshot.constraint_vector_ducv, &decoded->carriers.constraints,
          &error) ||
      !update_wire::DecodeAndValidateTypedUpdateTriggerVector(
          snapshot.trigger_vector_dutv, &decoded->carriers.triggers,
          &error) ||
      !update_wire::DecodeAndValidateTypedUpdateTargetOrder(
          snapshot.target_order_duor, &decoded->carriers.target_order,
          &error) ||
      !update_wire::DecodeAndValidateTypedUpdateResourceBudget(
          snapshot.resource_budget_dubr,
          &decoded->carriers.resource_budget, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateRecoveryToken(
          snapshot.recovery_token_durc,
          &decoded->carriers.recovery_token, &error) ||
      !update_wire::ValidateTypedUpdateCarrierSet(decoded->carriers,
                                                  &error) ||
      !update_wire::DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
          snapshot.source_policy_vector_dusv, &decoded->source_policies,
          &error) ||
      !update_wire::DecodeAndValidateTypedUpdateSecuritySnapshotProof(
          snapshot.security_snapshot_proof_dusp,
          &decoded->security_snapshot, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
          snapshot.datatype_authority_vector_dudv,
          &decoded->datatype_authority, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
          snapshot.builtin_operator_authority_vector_duov,
          &decoded->operator_authority, &error) ||
      !update_wire::ValidateTypedUpdateDatatypeOperatorAuthority(
          decoded->carriers.descriptor,
          decoded->carriers.assignments,
          decoded->carriers.predicate,
          decoded->datatype_authority,
          decoded->operator_authority, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateMgaRecoveryObservation(
          recovered.recovery_observation_dumo,
          &decoded->recovery_observation, &error)) {
    return fail(error, "sblr.dml_update_rows.recovery_carrier_invalid");
  }

  update_wire::TypedUpdateUuid expected_descriptor_uuid{};
  if (!DmlUpdateTypedUuid(descriptor_ref.descriptor_uuid,
                          &expected_descriptor_uuid) ||
      recovered.identity.database_uuid != context.database_uuid.canonical ||
      recovered.identity.authenticated_statement_receipt_uuid !=
          context.statement_receipt_uuid.canonical ||
      recovered.identity.owning_transaction_uuid !=
          context.transaction_uuid.canonical ||
      recovered.identity.owning_local_transaction_id !=
          context.local_transaction_id ||
      recovered.identity.descriptor_uuid != descriptor_ref.descriptor_uuid ||
      recovered.identity.descriptor_generation !=
          descriptor_ref.descriptor_generation ||
      decoded->carriers.descriptor.descriptor_uuid !=
          expected_descriptor_uuid ||
      decoded->carriers.descriptor.descriptor_generation !=
          descriptor_ref.descriptor_generation ||
      decoded->carriers.descriptor.structural_occurrence_id !=
          structural_occurrence_id) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.recovery_identity_stale");
    return false;
  }

  decoded->journal.clear();
  decoded->journal.reserve(recovered.journal.size());
  for (std::size_t index = 0; index < recovered.journal.size(); ++index) {
    const auto& extent = recovered.journal[index];
    const auto extent_state =
        DmlUpdateWireJournalStateV1(extent.lifecycle_state);
    if (!extent_state.has_value()) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_journal_state_invalid");
      return false;
    }
    update_wire::TypedUpdateJournalChainContext chain;
    chain.first_record = index == 0;
    if (index != 0) {
      const auto& prior = decoded->journal.back();
      chain.prior_sequence = prior.journal_sequence;
      chain.prior_state = prior.lifecycle_state;
      chain.prior_record_evidence_sha256 = prior.record_evidence_sha256;
      chain.require_same_descriptor = true;
      if (prior.embedded_descriptor_bytes.size() !=
          chain.expected_descriptor_bytes.size()) {
        *diagnostic = DmlUpdateDescriptorDiagnostic(
            "DML.UPDATE_FAILED",
            "sblr.dml_update_rows.recovery_descriptor_extent_invalid");
        return false;
      }
      std::copy(prior.embedded_descriptor_bytes.begin(),
                prior.embedded_descriptor_bytes.end(),
                chain.expected_descriptor_bytes.begin());
      if (prior.lifecycle_state ==
              update_wire::TypedUpdateJournalState::prepared &&
          prior.embedded_result_bytes.has_value()) {
        if (prior.embedded_result_bytes->size() !=
            update_wire::kTypedUpdateResultBytes) {
          *diagnostic = DmlUpdateDescriptorDiagnostic(
              "DML.UPDATE_FAILED",
              "sblr.dml_update_rows.recovery_result_extent_invalid");
          return false;
        }
        std::array<std::uint8_t, update_wire::kTypedUpdateResultBytes>
            expected_result{};
        std::copy(prior.embedded_result_bytes->begin(),
                  prior.embedded_result_bytes->end(),
                  expected_result.begin());
        chain.expected_prepared_result_bytes = expected_result;
      }
      if (prior.lifecycle_state == update_wire::TypedUpdateJournalState::intent ||
          prior.lifecycle_state ==
              update_wire::TypedUpdateJournalState::prepared) {
        chain.prior_savepoint_uuid = prior.statement_savepoint_uuid;
        chain.prior_savepoint_generation =
            prior.statement_savepoint_generation;
      } else if (
          prior.lifecycle_state == update_wire::TypedUpdateJournalState::bound &&
          *extent_state == update_wire::TypedUpdateJournalState::aborted &&
          decoded->recovery_observation.savepoint_state ==
              update_wire::TypedUpdateSavepointState::rolled_back_final) {
        chain.prior_savepoint_uuid =
            decoded->recovery_observation.statement_savepoint_uuid;
        chain.prior_savepoint_generation =
            decoded->recovery_observation.statement_savepoint_generation;
      }
    }
    update_wire::TypedUpdateJournalRecord journal;
    if (!update_wire::DecodeAndValidateTypedUpdateJournalRecord(
            extent.exact_dujr_bytes, chain, &journal, &error)) {
      return fail(error, "sblr.dml_update_rows.recovery_journal_invalid");
    }
    if (journal.journal_sequence != extent.journal_sequence ||
        journal.lifecycle_state != *extent_state ||
        journal.prior_record_sha256 != extent.prior_record_sha256 ||
        journal.record_evidence_sha256 != extent.record_evidence_sha256 ||
        journal.embedded_descriptor_bytes != snapshot.descriptor_dudc) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_journal_extent_mismatch");
      return false;
    }
    decoded->journal.push_back(std::move(journal));
  }
  decoded->staged_successor.reset();
  if (recovered.staged_successor_present) {
    const auto& head = decoded->journal.back();
    const auto& extent = recovered.staged_successor;
    if (head.lifecycle_state !=
            update_wire::TypedUpdateJournalState::prepared ||
        !head.embedded_result_bytes.has_value() ||
        head.embedded_result_bytes->size() !=
            update_wire::kTypedUpdateResultBytes ||
        extent.lifecycle_state !=
            MgaDmlUpdateDurableJournalStateV1::published ||
        extent.journal_sequence != head.journal_sequence + 1U ||
        extent.prior_record_sha256 != head.record_evidence_sha256) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_staged_successor_invalid");
      return false;
    }
    update_wire::TypedUpdateJournalChainContext chain;
    chain.first_record = false;
    chain.prior_sequence = head.journal_sequence;
    chain.prior_state = head.lifecycle_state;
    chain.prior_record_evidence_sha256 = head.record_evidence_sha256;
    chain.prior_savepoint_uuid = head.statement_savepoint_uuid;
    chain.prior_savepoint_generation = head.statement_savepoint_generation;
    chain.require_same_descriptor = true;
    if (head.embedded_descriptor_bytes.size() !=
        chain.expected_descriptor_bytes.size()) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_staged_descriptor_invalid");
      return false;
    }
    std::copy(head.embedded_descriptor_bytes.begin(),
              head.embedded_descriptor_bytes.end(),
              chain.expected_descriptor_bytes.begin());
    std::array<std::uint8_t, update_wire::kTypedUpdateResultBytes>
        expected_result{};
    std::copy(head.embedded_result_bytes->begin(),
              head.embedded_result_bytes->end(), expected_result.begin());
    chain.expected_prepared_result_bytes = expected_result;
    update_wire::TypedUpdateJournalRecord staged;
    if (!update_wire::DecodeAndValidateTypedUpdateJournalRecord(
            extent.exact_dujr_bytes, chain, &staged, &error)) {
      return fail(error,
                  "sblr.dml_update_rows.recovery_staged_successor_invalid");
    }
    if (staged.lifecycle_state !=
            update_wire::TypedUpdateJournalState::published ||
        staged.journal_sequence != extent.journal_sequence ||
        staged.prior_record_sha256 != extent.prior_record_sha256 ||
        staged.record_evidence_sha256 != extent.record_evidence_sha256 ||
        staged.embedded_descriptor_bytes != snapshot.descriptor_dudc ||
        !staged.embedded_result_bytes.has_value() ||
        staged.embedded_result_bytes != head.embedded_result_bytes) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_staged_extent_mismatch");
      return false;
    }
    decoded->staged_successor = std::move(staged);
  }
  if (!update_wire::ValidateTypedUpdateSecurityRecoveryAuthority(
          decoded->carriers.descriptor, decoded->carriers.row_policies,
          decoded->carriers.recovery_token, decoded->source_policies,
          decoded->security_snapshot, decoded->journal.back(),
          decoded->recovery_observation, &error)) {
    return fail(error, "sblr.dml_update_rows.recovery_authority_invalid");
  }
  const auto datatype_revalidation =
      RevalidateRecoveredDmlUpdateDatatypeOperatorAuthorityV1(
          context, decoded->carriers.descriptor,
          decoded->carriers.assignments, decoded->carriers.predicate,
          decoded->datatype_authority, decoded->operator_authority);
  if (datatype_revalidation.error) {
    *diagnostic = datatype_revalidation;
    return false;
  }
  const auto security_recovery =
      RecoverEngineSecurityPolicySnapshotFromValidatedDmlUpdateDurableAuthorityV1(
          context, recovered.validated_handle,
          recovered.recovery_observation_dumo);
  if (!security_recovery.ok) {
    *diagnostic = security_recovery.diagnostic.code.empty()
                      ? DmlUpdateDescriptorDiagnostic(
                            "DML.UPDATE_FAILED",
                            "sblr.dml_update_rows.recovery_security_authority_invalid")
                      : security_recovery.diagnostic;
    return false;
  }
  if (!update_wire::DecideTypedUpdateRecovery(
          decoded->recovery_observation, &decoded->decision, &error)) {
    return fail(error, "sblr.dml_update_rows.recovery_decision_invalid");
  }
  *diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return true;
}

bool DmlUpdateAppendRecoveredTerminalV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationRecoveryResultV1& recovered,
    const DmlUpdateDecodedDurableRecoveryV1& decoded,
    update_wire::TypedUpdateJournalState terminal_state,
    EngineApiDiagnostic* diagnostic) {
  if (diagnostic == nullptr || decoded.journal.empty() ||
      (terminal_state != update_wire::TypedUpdateJournalState::aborted &&
       terminal_state != update_wire::TypedUpdateJournalState::published)) {
    return false;
  }
  const auto& head = decoded.journal.back();
  update_wire::TypedUpdateJournalRecord terminal;
  terminal.lifecycle_state = terminal_state;
  terminal.journal_sequence = head.journal_sequence + 1U;
  terminal.database_uuid = head.database_uuid;
  terminal.authenticated_statement_receipt_uuid =
      head.authenticated_statement_receipt_uuid;
  terminal.owning_transaction_uuid = head.owning_transaction_uuid;
  terminal.owning_local_transaction_id = head.owning_local_transaction_id;
  terminal.operation_uuid = head.operation_uuid;
  terminal.recovery_token_uuid = head.recovery_token_uuid;
  terminal.recovery_generation = head.recovery_generation;
  terminal.prior_record_sha256 = head.record_evidence_sha256;
  terminal.descriptor = decoded.carriers.descriptor;
  if (terminal_state == update_wire::TypedUpdateJournalState::published) {
    if (head.lifecycle_state != update_wire::TypedUpdateJournalState::prepared ||
        !head.prior_result.has_value() ||
        !head.embedded_result_bytes.has_value() ||
        head.embedded_result_bytes->size() !=
            update_wire::kTypedUpdateResultBytes) {
      *diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_published_result_missing");
      return false;
    }
    terminal.statement_savepoint_uuid = head.statement_savepoint_uuid;
    terminal.statement_savepoint_generation =
        head.statement_savepoint_generation;
    terminal.prior_result = head.prior_result;
  } else if (decoded.recovery_observation.savepoint_state ==
             update_wire::TypedUpdateSavepointState::rolled_back_final) {
    terminal.statement_savepoint_uuid =
        decoded.recovery_observation.statement_savepoint_uuid;
    terminal.statement_savepoint_generation =
        decoded.recovery_observation.statement_savepoint_generation;
  }

  update_wire::TypedUpdateJournalChainContext chain;
  chain.first_record = false;
  chain.prior_sequence = head.journal_sequence;
  chain.prior_state = head.lifecycle_state;
  chain.prior_record_evidence_sha256 = head.record_evidence_sha256;
  chain.prior_savepoint_uuid = terminal.statement_savepoint_uuid;
  chain.prior_savepoint_generation = terminal.statement_savepoint_generation;
  chain.require_same_descriptor = true;
  if (head.embedded_descriptor_bytes.size() !=
      chain.expected_descriptor_bytes.size()) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.recovery_descriptor_extent_invalid");
    return false;
  }
  std::copy(head.embedded_descriptor_bytes.begin(),
            head.embedded_descriptor_bytes.end(),
            chain.expected_descriptor_bytes.begin());
  if (terminal_state == update_wire::TypedUpdateJournalState::published) {
    std::array<std::uint8_t, update_wire::kTypedUpdateResultBytes>
        expected_result{};
    std::copy(head.embedded_result_bytes->begin(),
              head.embedded_result_bytes->end(), expected_result.begin());
    chain.expected_prepared_result_bytes = expected_result;
  }

  update_wire::TypedUpdateCarrierError error;
  std::vector<std::uint8_t> exact_dujr;
  update_wire::TypedUpdateHash evidence{};
  if (!update_wire::EncodeTypedUpdateJournalRecord(
          terminal, chain, &exact_dujr, &error) ||
      !update_wire::ExtractTypedUpdateJournalRecordEvidence(
          exact_dujr, &evidence, &error)) {
    *diagnostic = DmlUpdateCarrierDiagnostic(
        error, "sblr.dml_update_rows.recovery_terminal_invalid");
    return false;
  }
  const auto prior_state =
      DmlUpdateDurableJournalStateV1(head.lifecycle_state);
  const auto next_state = DmlUpdateDurableJournalStateV1(terminal_state);
  if (!prior_state.has_value() || !next_state.has_value()) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.recovery_terminal_state_invalid");
    return false;
  }
  EngineDmlUpdateDurableAppendSuccessorRequestV1 request;
  request.context = context;
  request.append.identity = recovered.identity;
  request.append.expected_prior_sequence = head.journal_sequence;
  request.append.expected_prior_state = *prior_state;
  request.append.expected_prior_record_evidence_sha256 =
      head.record_evidence_sha256;
  request.append.successor.journal_sequence = terminal.journal_sequence;
  request.append.successor.lifecycle_state = *next_state;
  request.append.successor.prior_record_sha256 =
      head.record_evidence_sha256;
  request.append.successor.record_evidence_sha256 = evidence;
  request.append.successor.exact_dujr_bytes = std::move(exact_dujr);
  const auto appended =
      AppendDmlUpdateDurableOperationSuccessorV1(request);
  if (!appended.ok()) {
    *diagnostic = appended.diagnostic.code.empty()
                      ? DmlUpdateDescriptorDiagnostic(
                            "DML.UPDATE_FAILED",
                            "sblr.dml_update_rows.recovery_terminal_append_failed")
                      : appended.diagnostic;
    return false;
  }
  *diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return true;
}

MgaDmlUpdateDurableOperationIdentityV1 DmlUpdateDurableIdentityV1(
    const DmlUpdateRowsDescriptorRecordV1& record) {
  return record.durable_operation_identity;
}

MgaDmlUpdateDurableAuthoritySnapshotV1 DmlUpdateDurableAuthoritySnapshotV1(
    const DmlUpdateRowsDescriptorRecordV1& record) {
  const auto& carriers = record.canonical_carriers;
  MgaDmlUpdateDurableAuthoritySnapshotV1 snapshot;
  snapshot.assignment_vector_duav = carriers.assignments.exact_bytes;
  snapshot.predicate_vector_duev = carriers.predicate.exact_bytes;
  snapshot.row_policy_vector_dupv = carriers.row_policies.exact_bytes;
  snapshot.constraint_vector_ducv = carriers.constraints.exact_bytes;
  snapshot.trigger_vector_dutv = carriers.triggers.exact_bytes;
  snapshot.target_order_duor = carriers.target_order.exact_bytes;
  snapshot.resource_budget_dubr = carriers.resource_budget.exact_bytes;
  snapshot.recovery_token_durc = carriers.recovery_token.exact_bytes;
  snapshot.source_policy_vector_dusv =
      record.canonical_source_policies.exact_bytes;
  snapshot.security_snapshot_proof_dusp =
      record.canonical_security_snapshot.exact_bytes;
  snapshot.descriptor_dudc = carriers.descriptor.exact_bytes;
  snapshot.datatype_authority_vector_dudv =
      record.canonical_datatype_authority.exact_bytes;
  snapshot.builtin_operator_authority_vector_duov =
      record.canonical_operator_authority.exact_bytes;
  return snapshot;
}

std::optional<MgaDmlUpdateDurableJournalExtentV1>
DmlUpdateDurableJournalExtentV1(
    const DmlUpdatePreparedJournalAppendV1& prepared) {
  const auto state = DmlUpdateDurableJournalStateV1(prepared.next_state);
  if (!state.has_value() || prepared.exact_dujr_bytes.empty()) {
    return std::nullopt;
  }
  MgaDmlUpdateDurableJournalExtentV1 extent;
  extent.journal_sequence = prepared.expected_prior_sequence + 1U;
  extent.lifecycle_state = *state;
  extent.prior_record_sha256 = prepared.expected_prior_evidence_sha256;
  extent.record_evidence_sha256 = prepared.next_evidence_sha256;
  extent.exact_dujr_bytes = prepared.exact_dujr_bytes;
  return extent;
}

bool DmlUpdatePrepareJournalRecordV1(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record,
    DmlUpdatePreparedJournalAppendV1* prepared) {
  try {
    if (prepared == nullptr || context.database_path.empty() ||
        context.database_uuid.canonical != record.database_uuid) {
      return false;
    }
    std::vector<std::uint8_t> journal;
    update_wire::TypedUpdateJournalState next_state;
    update_wire::TypedUpdateHash next_evidence{};
    if (!DmlUpdateBuildJournalRecordV1(
            context, record, &journal, &next_state, &next_evidence)) {
      return false;
    }
    prepared->exact_dujr_bytes = std::move(journal);
    prepared->expected_prior_sequence = record.journal_sequence;
    prepared->expected_prior_state = record.latest_journal_state;
    prepared->expected_prior_evidence_sha256 =
        record.latest_journal_evidence_sha256;
    prepared->next_state = next_state;
    prepared->next_evidence_sha256 = next_evidence;
    return true;
  } catch (...) {
    return false;
  }
}

bool DmlUpdateCommitPreparedJournalRecordV1(
    const EngineRequestContext& context,
    DmlUpdateRowsDescriptorRecordV1* record,
    const DmlUpdatePreparedJournalAppendV1& prepared) {
  if (record == nullptr || prepared.exact_dujr_bytes.empty() ||
      record->journal_sequence != prepared.expected_prior_sequence ||
      record->latest_journal_state != prepared.expected_prior_state ||
      record->latest_journal_evidence_sha256 !=
          prepared.expected_prior_evidence_sha256) {
    return false;
  }
  const auto extent = DmlUpdateDurableJournalExtentV1(prepared);
  if (!extent.has_value()) return false;
  MgaDmlUpdateDurableOperationMutationResultV1 mutation;
  if (prepared.expected_prior_sequence == 0) {
    EngineDmlUpdateDurablePublishBoundRequestV1 request;
    request.context = context;
    request.publication.identity = DmlUpdateDurableIdentityV1(*record);
    request.publication.authority_snapshot =
        DmlUpdateDurableAuthoritySnapshotV1(*record);
    request.publication.bound_journal = *extent;
    mutation = PublishDmlUpdateDurableOperationBoundV1(request);
  } else {
    const auto prior_state =
        DmlUpdateDurableJournalStateV1(prepared.expected_prior_state);
    if (!prior_state.has_value()) return false;
    EngineDmlUpdateDurableAppendSuccessorRequestV1 request;
    request.context = context;
    request.append.identity = DmlUpdateDurableIdentityV1(*record);
    request.append.expected_prior_sequence =
        prepared.expected_prior_sequence;
    request.append.expected_prior_state = *prior_state;
    request.append.expected_prior_record_evidence_sha256 =
        prepared.expected_prior_evidence_sha256;
    request.append.successor = *extent;
    mutation = AppendDmlUpdateDurableOperationSuccessorV1(request);
  }
  if (!mutation.ok()) return false;
  record->journal_sequence += 1U;
  record->latest_journal_state = prepared.next_state;
  record->latest_journal_evidence_sha256 =
      prepared.next_evidence_sha256;
  return true;
}

bool DmlUpdatePrepareDurableSuccessorV1(
    const EngineRequestContext& context,
    const DmlUpdateRowsDescriptorRecordV1& record,
    const DmlUpdatePreparedJournalAppendV1& prepared,
    DmlUpdatePreparedDurableSuccessorV1* durable_prepared) {
  if (durable_prepared == nullptr || prepared.expected_prior_sequence == 0 ||
      prepared.exact_dujr_bytes.empty() ||
      record.journal_sequence != prepared.expected_prior_sequence ||
      record.latest_journal_state != prepared.expected_prior_state ||
      record.latest_journal_evidence_sha256 !=
          prepared.expected_prior_evidence_sha256) {
    return false;
  }
  const auto prior_state =
      DmlUpdateDurableJournalStateV1(prepared.expected_prior_state);
  const auto extent = DmlUpdateDurableJournalExtentV1(prepared);
  if (!prior_state.has_value() || !extent.has_value()) return false;

  EngineDmlUpdateDurableAppendSuccessorRequestV1 request;
  request.context = context;
  request.append.identity = DmlUpdateDurableIdentityV1(record);
  request.append.expected_prior_sequence = prepared.expected_prior_sequence;
  request.append.expected_prior_state = *prior_state;
  request.append.expected_prior_record_evidence_sha256 =
      prepared.expected_prior_evidence_sha256;
  request.append.successor = *extent;
  auto provider_prepared =
      PrepareDmlUpdateDurableOperationSuccessorV1(request);
  if (!provider_prepared.ok()) return false;

  durable_prepared->expected_prior_sequence =
      prepared.expected_prior_sequence;
  durable_prepared->expected_prior_state = prepared.expected_prior_state;
  durable_prepared->expected_prior_evidence_sha256 =
      prepared.expected_prior_evidence_sha256;
  durable_prepared->next_state = prepared.next_state;
  durable_prepared->next_evidence_sha256 = prepared.next_evidence_sha256;
  durable_prepared->provider_prepared =
      std::move(provider_prepared.prepared);
  return durable_prepared->provider_prepared.valid();
}

bool DmlUpdateCommitDurableSuccessorNoBuildV1(
    DmlUpdateRowsDescriptorRecordV1* record,
    DmlUpdatePreparedDurableSuccessorV1&& prepared) {
  if (record == nullptr || !prepared.provider_prepared.valid() ||
      record->journal_sequence != prepared.expected_prior_sequence ||
      record->latest_journal_state != prepared.expected_prior_state ||
      record->latest_journal_evidence_sha256 !=
          prepared.expected_prior_evidence_sha256) {
    return false;
  }
  const auto mutation = CommitPreparedDmlUpdateDurableOperationSuccessorV1(
      std::move(prepared.provider_prepared));
  if (!mutation.ok()) return false;
  record->journal_sequence += 1U;
  record->latest_journal_state = prepared.next_state;
  record->latest_journal_evidence_sha256 = prepared.next_evidence_sha256;
  return true;
}

void DmlUpdateCopyCommittedJournalPositionV1(
    const DmlUpdateRowsDescriptorRecordV1& committed,
    DmlUpdateRowsDescriptorRecordV1* replacement) {
  if (replacement == nullptr) return;
  replacement->journal_sequence = committed.journal_sequence;
  replacement->latest_journal_state = committed.latest_journal_state;
  replacement->latest_journal_evidence_sha256 =
      committed.latest_journal_evidence_sha256;
}

bool DmlUpdatePublishJournalRecordV1(
    const EngineRequestContext& context,
    DmlUpdateRowsDescriptorRecordV1& record) {
  DmlUpdatePreparedJournalAppendV1 prepared;
  return DmlUpdatePrepareJournalRecordV1(context, record, &prepared) &&
         DmlUpdateCommitPreparedJournalRecordV1(context, &record, prepared);
}

std::vector<std::uint8_t> EncodeDmlUpdateRowsResultV1(
    const DmlUpdateRowsDescriptorRecordV1& record,
    const EngineUpdateRowsResult& result) {
  if (!result.ok || result.updated_count > result.matched_count ||
      record.descriptor_ref.descriptor_generation == 0 ||
      record.operation_generation == 0 || record.local_transaction_id == 0 ||
      record.relation_generation == 0 ||
      record.publication_barrier_generation == 0) {
    return {};
  }
  update_wire::TypedUpdateResultCarrier carrier;
  if (!DmlUpdateTypedUuid(record.descriptor_ref.descriptor_uuid,
                          &carrier.update_descriptor_uuid) ||
      !DmlUpdateTypedUuid(record.operation_uuid, &carrier.operation_uuid) ||
      !DmlUpdateTypedUuid(record.transaction_uuid,
                          &carrier.owning_transaction_uuid) ||
      !DmlUpdateTypedUuid(record.relation_uuid, &carrier.relation_uuid) ||
      !DmlUpdateTypedUuid(record.publication_barrier_uuid,
                          &carrier.publication_barrier_uuid)) {
    return {};
  }
  carrier.update_descriptor_generation =
      record.descriptor_ref.descriptor_generation;
  carrier.owning_local_transaction_id = record.local_transaction_id;
  carrier.relation_generation = record.relation_generation;
  carrier.matched_count = result.matched_count;
  carrier.updated_count = result.updated_count;
  carrier.publication_barrier_generation =
      record.publication_barrier_generation;

  std::vector<update_wire::TypedUpdateResultEvidenceReference> evidence;
  evidence.reserve(result.evidence.size());
  for (const auto& source : result.evidence) {
    evidence.push_back({source.evidence_kind, source.evidence_id});
  }
  update_wire::TypedUpdateCarrierError error;
  if (!update_wire::ComputeTypedUpdateResultInnerEvidence(
          carrier, evidence, &carrier.effect_set_sha256,
          &carrier.executor_evidence_sha256, &error)) {
    return {};
  }
  std::vector<std::uint8_t> encoded;
  update_wire::TypedUpdateResultCarrier decoded;
  if (!update_wire::EncodeTypedUpdateResult(carrier, &encoded, &error) ||
      !update_wire::DecodeAndValidateTypedUpdateResult(
          encoded, &decoded, &error) || decoded.exact_bytes != encoded) {
    return {};
  }
  return encoded;
}

}  // namespace

void SetDmlUpdateRowsTestFaultPointV1(
    EngineDmlUpdateRowsTestFaultPointV1 fault_point) {
  g_dml_update_test_fault_point.store(fault_point,
                                      std::memory_order_release);
}

void ResetDmlUpdateRowsDescriptorRegistryForTestV1() {
  std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
  g_dml_update_descriptors.clear();
  g_dml_update_test_fault_point.store(
      EngineDmlUpdateRowsTestFaultPointV1::none,
      std::memory_order_release);
}

EngineDmlUpdateRowsBindResultV1 BindDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsBindingDemandV1& demand) {
  EngineDmlUpdateRowsBindResultV1 result;
  const auto refuse = [&](std::string code, std::string key,
                          std::string detail = {}) {
    result.diagnostic = DmlUpdateDescriptorDiagnostic(
        std::move(code), std::move(key), std::move(detail));
    return result;
  };
  if (!context.security_context_present ||
      !context.statement_metadata_snapshot_engine_owned ||
      !context.authorization_context.present ||
      context.authorization_context.authority_uuid.canonical.empty() ||
      context.authorization_context.security_context_generation == 0 ||
      !DmlUpdateHasTraceTag(context, "private_dml_update_rows_binder")) {
    return refuse("SECURITY.ACCESS_DENIED",
                  "sblr.dml_update_rows.binding_authority_required");
  }
  if (context.read_only_mode || context.local_transaction_id == 0 ||
      context.transaction_uuid.canonical.empty() ||
      context.statement_receipt_uuid.canonical.empty() ||
      context.statement_receipt_uuid.canonical !=
          demand.authenticated_statement_receipt_uuid ||
      context.datatype_catalog_snapshot_uuid.canonical.empty() ||
      context.datatype_catalog_generation == 0 ||
      context.datatype_registry_generation == 0 ||
      demand.structural_occurrence_id == 0 ||
      demand.target_relation_uuid_hint.empty() || demand.assignments.empty() ||
      demand.assignments.size() > 1024) {
    return refuse("MGA.TRANSACTION.STALE",
                  "sblr.dml_update_rows.binding_context_stale");
  }
  if (DmlUpdateCancellationRequested(context)) {
    return refuse("PROCESS.CANCELLED",
                  "sblr.dml_update_rows.binding_cancelled");
  }
  const auto loaded = TransactionalRelationStore(context).LoadRelationDescriptor(
      demand.target_relation_uuid_hint);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  const auto availability = LoadSblrExecutorAvailabilitySnapshot(
      context, DmlUpdateRowsExecutorIdentity());
  if (!availability.ok) {
    result.diagnostic = availability.diagnostic;
    return result;
  }
  SblrExecutorAvailabilitySnapshot current_availability;
  const auto availability_diagnostic = RevalidateSblrExecutorAvailability(
      context, DmlUpdateRowsExecutorIdentity(), availability.snapshot,
      &current_availability);
  if (availability_diagnostic.error ||
      current_availability.generation == 0) {
    result.diagnostic = availability_diagnostic;
    return result;
  }

  DmlUpdateRowsDescriptorRecordV1 record;
  record.statement_receipt_uuid = context.statement_receipt_uuid.canonical;
  record.structural_occurrence_id = demand.structural_occurrence_id;
  record.database_uuid = context.database_uuid.canonical;
  record.session_uuid = context.session_uuid.canonical;
  record.transaction_uuid = context.transaction_uuid.canonical;
  record.local_transaction_id = context.local_transaction_id;
  record.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  record.metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  record.datatype_catalog_snapshot_uuid =
      context.datatype_catalog_snapshot_uuid.canonical;
  record.datatype_catalog_generation = context.datatype_catalog_generation;
  record.datatype_registry_generation = context.datatype_registry_generation;
  record.security_epoch = context.security_epoch;
  record.catalog_generation_id = context.catalog_generation_id;
  record.executor_availability_snapshot = current_availability;
  record.relation_uuid = loaded.descriptor.relation_uuid.canonical;
  record.relation_generation = loaded.descriptor.relation_generation;
  record.relation_descriptor_uuid =
      loaded.descriptor.descriptor_uuid.canonical;
  record.relation_descriptor_generation =
      loaded.descriptor.descriptor_generation;
  record.prepared_request.context = context;
  record.prepared_request.operation_id = "dml.update_rows";
  record.prepared_request.target_table.uuid.canonical = record.relation_uuid;
  record.prepared_request.target_table.object_kind = "table";
  record.prepared_request.option_envelopes.push_back(
      "result_payload_policy:summary_only");

  std::unordered_set<std::string> assigned_column_uuids;
  std::uint32_t expected_ordinal = 1;
  for (const auto& assignment : demand.assignments) {
    if (assignment.ordinal != expected_ordinal++ ||
        assignment.target_column_spelling.empty() ||
        assignment.literal_spelling.empty()) {
      return refuse("SBLR.OPERAND_INVALID",
                    "sblr.dml_update_rows.assignment_invalid");
    }
    const auto* column = DmlUpdateFindColumn(
        loaded.descriptor, assignment.target_column_spelling);
    if (column == nullptr || column->generated ||
        !assigned_column_uuids.insert(column->column_uuid.canonical).second) {
      return refuse("DML.ASSIGNMENT_SHAPE_INVALID",
                    "sblr.dml_update_rows.assignment_column_refused",
                    assignment.target_column_spelling);
    }
    EngineTypedValue value;
    DmlUpdateBoundColumnV1 identity;
    EngineApiDiagnostic diagnostic;
    if (!DmlUpdateBindLiteral(context, *column,
                              assignment.literal_spelling, &value,
                              &identity, &diagnostic)) {
      result.diagnostic = std::move(diagnostic);
      return result;
    }
    record.prepared_request.assignments.push_back(
        {column->canonical_name_key, std::move(value)});
    record.assignment_columns.push_back(std::move(identity));
  }

  if (demand.predicate_kind.empty()) {
    record.prepared_request.update_predicate.predicate_kind = "engine_bound_true";
  } else {
    if (demand.predicate_kind != "column_equals" ||
        demand.predicate_column_spelling.empty() ||
        demand.predicate_literal_spelling.empty()) {
      return refuse("SBLR.OPERAND_INVALID",
                    "sblr.dml_update_rows.predicate_invalid");
    }
    const auto* column = DmlUpdateFindColumn(
        loaded.descriptor, demand.predicate_column_spelling);
    if (column == nullptr) {
      return refuse("SBLR.OPERAND_INVALID",
                    "sblr.dml_update_rows.predicate_column_refused",
                    demand.predicate_column_spelling);
    }
    EngineTypedValue value;
    DmlUpdateBoundColumnV1 identity;
    EngineApiDiagnostic diagnostic;
    if (!DmlUpdateBindLiteral(context, *column,
                              demand.predicate_literal_spelling, &value,
                              &identity, &diagnostic)) {
      result.diagnostic = std::move(diagnostic);
      return result;
    }
    record.prepared_request.update_predicate.predicate_kind = "column_equals";
    record.prepared_request.update_predicate.canonical_predicate_envelope =
        column->canonical_name_key;
    record.prepared_request.update_predicate.bound_values.push_back(
        std::move(value));
    record.predicate_column = std::move(identity);
  }

  if (!DmlUpdateIssueIdentity(&record.descriptor_ref.descriptor_uuid) ||
      !DmlUpdateIssueIdentity(&record.operation_uuid) ||
      !DmlUpdateIssueIdentity(&record.relation_occurrence_uuid)) {
    return refuse("SBLR.OPERAND_INVALID",
                  "sblr.dml_update_rows.descriptor_identity_unavailable");
  }
  record.descriptor_ref.descriptor_generation = 1;
  record.operation_generation = 1;
  record.relation_occurrence_generation = 1;

  EngineDmlUpdatePolicyCatalogCaptureRequestV1 policy_capture_request;
  policy_capture_request.context = context;
  policy_capture_request.authenticated_statement_receipt_uuid =
      record.statement_receipt_uuid;
  policy_capture_request.structural_occurrence_id =
      record.structural_occurrence_id;
  policy_capture_request.relation_occurrence.relation_uuid =
      record.relation_uuid;
  policy_capture_request.relation_occurrence.relation_generation =
      record.relation_generation;
  policy_capture_request.relation_occurrence.relation_occurrence_uuid =
      record.relation_occurrence_uuid;
  policy_capture_request.relation_occurrence
      .relation_occurrence_generation = record.relation_occurrence_generation;
  policy_capture_request.catalog_snapshot_uuid =
      record.metadata_snapshot_uuid;
  policy_capture_request.catalog_generation = record.catalog_generation_id;
  policy_capture_request.descriptor_uuid =
      record.descriptor_ref.descriptor_uuid;
  policy_capture_request.descriptor_generation =
      record.descriptor_ref.descriptor_generation;
  auto policy_authority =
      CaptureDmlUpdatePolicyCatalogAuthorityV1(policy_capture_request);
  if (!policy_authority.ok) {
    result.diagnostic = policy_authority.diagnostic;
    return result;
  }
  record.policy_catalog_authority = std::move(policy_authority);

  EngineDmlUpdateDatatypeOperatorBindingRequestV1 datatype_binding_request;
  datatype_binding_request.context = context;
  datatype_binding_request.equality_required =
      record.predicate_column.has_value();
  if (record.predicate_column.has_value()) {
    const auto& predicate_identity = *record.predicate_column;
    datatype_binding_request.left_descriptor_uuid =
        predicate_identity.datatype_descriptor_uuid;
    datatype_binding_request.left_descriptor_generation =
        predicate_identity.datatype_descriptor_generation;
    datatype_binding_request.left_type_uuid = predicate_identity.type_uuid;
    datatype_binding_request.left_type_generation =
        predicate_identity.type_generation;
    datatype_binding_request.right_descriptor_uuid =
        predicate_identity.datatype_descriptor_uuid;
    datatype_binding_request.right_descriptor_generation =
        predicate_identity.datatype_descriptor_generation;
    datatype_binding_request.right_type_uuid = predicate_identity.type_uuid;
    datatype_binding_request.right_type_generation =
        predicate_identity.type_generation;
  }
  record.datatype_operator_binding =
      ResolveDmlUpdateDatatypeOperatorBindingAuthorityV1(
          datatype_binding_request);
  if (!record.datatype_operator_binding.ok) {
    result.diagnostic = record.datatype_operator_binding.diagnostic;
    return result;
  }

  const auto freeze_request =
      DmlUpdateCurrentImmutableAuthorityRequest(context, record);
  const auto frozen = FreezeDmlUpdateImmutableAuthorityV1(freeze_request);
  if (!frozen.ok) {
    result.diagnostic = frozen.diagnostic;
    return result;
  }
  record.immutable_authority_snapshot = frozen.snapshot;
  record.security_context_uuid =
      frozen.snapshot.security_policy_snapshot.security_context_uuid;
  record.security_context_generation =
      frozen.snapshot.security_policy_snapshot.security_context_generation;
  record.security_snapshot_uuid =
      frozen.snapshot.security_policy_snapshot.snapshot_uuid;
  record.security_generation =
      frozen.snapshot.security_policy_snapshot.security_generation;

  EngineApiDiagnostic carrier_diagnostic;
  if (!DmlUpdateBuildAssignmentCarrier(&record, &carrier_diagnostic) ||
      !DmlUpdateBuildPredicateCarrier(&record, &carrier_diagnostic) ||
      !DmlUpdateBuildFrozenAuthorityCarriers(
          frozen.snapshot, &record, &carrier_diagnostic)) {
    result.diagnostic = carrier_diagnostic.code.empty()
                            ? DmlUpdateDescriptorDiagnostic(
                                  "SBLR.OPERAND_INVALID",
                                  "sblr.dml_update_rows.carrier_build_failed")
                            : carrier_diagnostic;
    return result;
  }
  if (!DmlUpdateBuildExecutionAuthorityCarriers(
          context, &record, &carrier_diagnostic)) {
    result.diagnostic = carrier_diagnostic.code.empty()
                            ? DmlUpdateDescriptorDiagnostic(
                                  "SBLR.OPERAND_INVALID",
                                  "sblr.dml_update_rows.carrier_build_failed")
                            : carrier_diagnostic;
    return result;
  }
  if (!DmlUpdateBuildDescriptorCarrier(&record, &carrier_diagnostic)) {
    const bool abandoned =
        DmlUpdateAbandonDurableReservationV1(context, record);
    result.diagnostic = !abandoned
                            ? DmlUpdateDescriptorDiagnostic(
                                  "DML.UPDATE_FAILED",
                                  "sblr.dml_update_rows.reservation_abandon_failed")
                            : carrier_diagnostic.code.empty()
                                  ? DmlUpdateDescriptorDiagnostic(
                                        "SBLR.OPERAND_INVALID",
                                        "sblr.dml_update_rows.carrier_build_failed")
                                  : carrier_diagnostic;
    return result;
  }
  if (!DmlUpdateCaptureCanonicalProviderAuthorities(
          context, &record, &carrier_diagnostic)) {
    const bool abandoned =
        DmlUpdateAbandonDurableReservationV1(context, record);
    result.diagnostic = !abandoned
                            ? DmlUpdateDescriptorDiagnostic(
                                  "DML.UPDATE_FAILED",
                                  "sblr.dml_update_rows.reservation_abandon_failed")
                            : carrier_diagnostic.code.empty()
                                  ? DmlUpdateDescriptorDiagnostic(
                                        "SBLR.OPERAND_INVALID",
                                        "sblr.dml_update_rows.provider_authority_capture_failed")
                                  : carrier_diagnostic;
    return result;
  }
  // Allocate the caller result and private map node before bound durability.
  // The mutex keeps the uncommitted node invisible to descriptor consumers;
  // once PublishBound commits, no fallible host allocation is needed to expose
  // the already-durable reference.
  try {
    result.descriptor_ref = record.descriptor_ref;
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
    if (g_dml_update_descriptors.contains(
            record.descriptor_ref.descriptor_uuid)) {
      (void)DmlUpdateAbandonDurableReservationV1(context, record);
      return refuse("SBLR.OPERAND_INVALID",
                    "sblr.dml_update_rows.descriptor_identity_collision");
    }
    auto [stored, inserted] = g_dml_update_descriptors.emplace(
        record.descriptor_ref.descriptor_uuid, std::move(record));
    if (!inserted) {
      return refuse("SBLR.OPERAND_INVALID",
                    "sblr.dml_update_rows.descriptor_identity_collision");
    }
    bool published = false;
    try {
      published = DmlUpdatePublishJournalRecordV1(context, stored->second);
    } catch (...) {
      (void)DmlUpdateAbandonDurableReservationV1(context, stored->second);
      g_dml_update_descriptors.erase(stored);
      throw;
    }
    if (!published) {
      const bool abandoned =
          DmlUpdateAbandonDurableReservationV1(context, stored->second);
      g_dml_update_descriptors.erase(stored);
      return refuse(
          "DML.UPDATE_FAILED",
          abandoned ? "sblr.dml_update_rows.registry_publish_failed"
                    : "sblr.dml_update_rows.reservation_abandon_failed");
    }
  } catch (const std::bad_alloc&) {
    (void)DmlUpdateAbandonDurableReservationV1(context, record);
    return refuse("RESOURCE.BUDGET_EXCEEDED",
                  "sblr.dml_update_rows.binding_publication_allocation_failed");
  } catch (...) {
    (void)DmlUpdateAbandonDurableReservationV1(context, record);
    return refuse("DML.UPDATE_FAILED",
                  "sblr.dml_update_rows.binding_publication_failed");
  }
  result.ok = true;
  return result;
}

bool DmlUpdateBuildImmutableReplayV1(
    const update_wire::TypedUpdateJournalRecord& journal,
    EngineDmlUpdateRowsConsumeResultV1* result) {
  if (result == nullptr ||
      journal.lifecycle_state !=
          update_wire::TypedUpdateJournalState::published ||
      !journal.prior_result.has_value() ||
      !journal.embedded_result_bytes.has_value() ||
      journal.embedded_result_bytes->size() !=
          update_wire::kTypedUpdateResultBytes) {
    return false;
  }
  result->prior_result = {};
  result->prior_result.ok = true;
  result->prior_result.operation_id = "dml.update_rows";
  result->prior_result.matched_count = journal.prior_result->matched_count;
  result->prior_result.updated_count = journal.prior_result->updated_count;
  // Host evidence is deliberately not reconstructed.  The exact DURS inner
  // hashes remain the durable executor/effect evidence and are returned as
  // canonical bytes to the public result path.
  result->canonical_result_bytes = *journal.embedded_result_bytes;
  result->immutable_replay = true;
  result->ok = true;
  result->diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return true;
}

bool DmlUpdateRevalidateRecoveredSecurityV1(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationRecoveryResultV1& recovered,
    const DmlUpdateDecodedDurableRecoveryV1& decoded,
    EngineApiDiagnostic* diagnostic) {
  if (diagnostic == nullptr || !recovered.validated_handle.valid() ||
      recovered.recovery_observation_dumo.empty()) {
    return false;
  }
  const auto security =
      RecoverEngineSecurityPolicySnapshotFromValidatedDmlUpdateDurableAuthorityV1(
          context, recovered.validated_handle,
          recovered.recovery_observation_dumo);
  if (!security.ok) {
    *diagnostic = security.diagnostic.code.empty()
                      ? DmlUpdateDescriptorDiagnostic(
                            "DML.UPDATE_FAILED",
                            "sblr.dml_update_rows.recovery_security_invalid")
                      : security.diagnostic;
    return false;
  }
  if (security.snapshot.snapshot_uuid !=
          DmlUpdateUuidText(decoded.security_snapshot.security_snapshot_uuid) ||
      security.snapshot.snapshot_generation !=
          decoded.security_snapshot.security_snapshot_generation ||
      security.snapshot.security_context_uuid !=
          DmlUpdateUuidText(decoded.security_snapshot.security_context_uuid) ||
      security.snapshot.security_context_generation !=
          decoded.security_snapshot.security_context_generation ||
      security.snapshot.security_generation !=
          decoded.security_snapshot.security_epoch ||
      security.snapshot.policy_generation !=
          decoded.security_snapshot.policy_generation ||
      security.snapshot.authenticated_statement_receipt_uuid !=
          context.statement_receipt_uuid.canonical ||
      security.snapshot.target_relation_uuid !=
          DmlUpdateUuidText(decoded.security_snapshot.target_relation_uuid)) {
    *diagnostic = DmlUpdateDescriptorDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.recovery_security_binding_mismatch");
    return false;
  }
  return true;
}

EngineDmlUpdateRowsConsumeResultV1 DmlUpdateConsumeRecoveredDurableV1(
    const EngineRequestContext& consumer_context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    std::uint64_t structural_occurrence_id) {
  auto context = consumer_context;
  context.trace_tags.erase(
      std::remove(context.trace_tags.begin(), context.trace_tags.end(),
                  "private_dml_update_rows_consumer"),
      context.trace_tags.end());
  context.trace_tags.erase(
      std::remove(context.trace_tags.begin(), context.trace_tags.end(),
                  "private_dml_update_rows_binder"),
      context.trace_tags.end());
  if (!DmlUpdateHasTraceTag(context,
                            "private_dml_update_rows_recovery")) {
    context.trace_tags.push_back("private_dml_update_rows_recovery");
  }
  EngineDmlUpdateRowsConsumeResultV1 result;
  if (descriptor_ref.descriptor_uuid.empty() ||
      descriptor_ref.descriptor_generation == 0 ||
      structural_occurrence_id == 0) {
    result.diagnostic = DmlUpdateDescriptorDiagnostic(
        "SBLR.OPERAND_INVALID",
        "sblr.dml_update_rows.recovery_lookup_invalid");
    return result;
  }
  EngineDmlUpdateDurableRecoverChainRequestV1 request;
  request.context = context;
  request.lookup.descriptor_uuid = descriptor_ref.descriptor_uuid;
  request.lookup.descriptor_generation =
      descriptor_ref.descriptor_generation;
  request.lookup.structural_occurrence_id = structural_occurrence_id;
  auto recovered = RecoverDmlUpdateDurableOperationChainV1(request);
  if (!recovered.ok()) {
    result.diagnostic = recovered.diagnostic.code.empty()
                            ? DmlUpdateDescriptorDiagnostic(
                                  "SECURITY.ACCESS_DENIED",
                                  "sblr.dml_update_rows.descriptor_hidden")
                            : recovered.diagnostic;
    return result;
  }
  DmlUpdateDecodedDurableRecoveryV1 decoded;
  EngineApiDiagnostic diagnostic;
  if (!DmlUpdateDecodeDurableRecoveryV1(
          context, descriptor_ref, structural_occurrence_id, recovered,
          &decoded, &diagnostic) ||
      !DmlUpdateRevalidateRecoveredSecurityV1(
          context, recovered, decoded, &diagnostic)) {
    result.diagnostic = diagnostic.code.empty()
                            ? DmlUpdateDescriptorDiagnostic(
                                  "DML.UPDATE_FAILED",
                                  "sblr.dml_update_rows.recovery_invalid")
                            : std::move(diagnostic);
    return result;
  }

  if (decoded.decision ==
      update_wire::TypedUpdateRecoveryDecision::append_aborted_no_result) {
    if (decoded.recovery_observation.savepoint_state ==
        update_wire::TypedUpdateSavepointState::active) {
      const auto rolled_back =
          RollbackDmlUpdateStatementFromValidatedDurableAuthorityV1(
              context, recovered.validated_handle);
      if (!rolled_back.ok()) {
        result.diagnostic = rolled_back.diagnostic.code.empty()
                                ? DmlUpdateDescriptorDiagnostic(
                                      "MGA.TRANSACTION.ROLLBACK_FAILED",
                                      "sblr.dml_update_rows.recovery_rollback_failed")
                                : rolled_back.diagnostic;
        return result;
      }
      auto refreshed = RecoverDmlUpdateDurableOperationChainV1(request);
      DmlUpdateDecodedDurableRecoveryV1 refreshed_decoded;
      if (!refreshed.ok() ||
          !DmlUpdateDecodeDurableRecoveryV1(
              context, descriptor_ref, structural_occurrence_id, refreshed,
              &refreshed_decoded, &diagnostic) ||
          !DmlUpdateRevalidateRecoveredSecurityV1(
              context, refreshed, refreshed_decoded, &diagnostic) ||
          refreshed_decoded.decision !=
              update_wire::TypedUpdateRecoveryDecision::
                  append_aborted_no_result ||
          refreshed_decoded.recovery_observation.savepoint_state !=
              update_wire::TypedUpdateSavepointState::rolled_back_final) {
        result.diagnostic = diagnostic.code.empty()
                                ? DmlUpdateDescriptorDiagnostic(
                                      "DML.UPDATE_FAILED",
                                      "sblr.dml_update_rows.recovery_rollback_observation_invalid")
                                : std::move(diagnostic);
        return result;
      }
      recovered = std::move(refreshed);
      decoded = std::move(refreshed_decoded);
    }
    if (!DmlUpdateAppendRecoveredTerminalV1(
            context, recovered, decoded,
            update_wire::TypedUpdateJournalState::aborted, &diagnostic)) {
      result.diagnostic = diagnostic.code.empty()
                              ? DmlUpdateDescriptorDiagnostic(
                                    "DML.UPDATE_FAILED",
                                    "sblr.dml_update_rows.recovery_abort_publish_failed")
                              : std::move(diagnostic);
      return result;
    }
    result.diagnostic = DmlUpdateDescriptorDiagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.recovered_prepublication_abort");
    return result;
  }

  if (decoded.decision ==
      update_wire::TypedUpdateRecoveryDecision::
          append_published_and_replay_result) {
    if (!recovered.staged_successor_present ||
        !decoded.staged_successor.has_value()) {
      result.diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_staged_successor_missing");
      return result;
    }
    const auto committed =
        CommitRecoveredDmlUpdateDurableOperationStagedSuccessorV1(
            context, recovered.validated_handle);
    if (!committed.ok()) {
      result.diagnostic = committed.diagnostic.code.empty()
                              ? DmlUpdateDescriptorDiagnostic(
                                    "DML.UPDATE_FAILED",
                                    "sblr.dml_update_rows.recovery_published_append_failed",
                                    "known_applied_recovery_required")
                              : committed.diagnostic;
      return result;
    }
    if (!DmlUpdateBuildImmutableReplayV1(
            *decoded.staged_successor, &result)) {
      result = {};
      result.diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_result_invalid");
    }
    return result;
  }

  if (decoded.decision ==
      update_wire::TypedUpdateRecoveryDecision::replay_published_result) {
    if (!DmlUpdateBuildImmutableReplayV1(decoded.journal.back(), &result)) {
      result.diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.recovery_result_invalid");
    }
    return result;
  }

  result.diagnostic = DmlUpdateDescriptorDiagnostic(
      decoded.decision ==
              update_wire::TypedUpdateRecoveryDecision::stale_replay
          ? "MGA.TRANSACTION.STALE"
          : "DML.UPDATE_FAILED",
      decoded.decision ==
              update_wire::TypedUpdateRecoveryDecision::stale_replay
          ? "sblr.dml_update_rows.recovery_stale"
          : "sblr.dml_update_rows.recovery_quarantined");
  return result;
}

EngineDmlUpdateRowsConsumeResultV1 ConsumeDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    std::uint64_t structural_occurrence_id) {
  EngineDmlUpdateRowsConsumeResultV1 result;
  std::unique_lock<std::mutex> guard(g_dml_update_descriptor_mutex);
  auto found = g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
  if (found == g_dml_update_descriptors.end()) {
    guard.unlock();
    return DmlUpdateConsumeRecoveredDurableV1(
        context, descriptor_ref, structural_occurrence_id);
  }
  if (found->second.statement_receipt_uuid !=
          context.statement_receipt_uuid.canonical ||
      found->second.database_uuid != context.database_uuid.canonical) {
    result.diagnostic = DmlUpdateDescriptorDiagnostic(
        "SECURITY.ACCESS_DENIED",
        "sblr.dml_update_rows.descriptor_cross_authority_refused");
    return result;
  }
  if (descriptor_ref.descriptor_generation != 1 ||
      found->second.descriptor_ref.descriptor_generation !=
          descriptor_ref.descriptor_generation ||
      structural_occurrence_id == 0 ||
      found->second.structural_occurrence_id != structural_occurrence_id ||
      !DmlUpdateContextMatches(context, found->second)) {
    result.diagnostic = DmlUpdateDescriptorDiagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.descriptor_stale");
    return result;
  }
  auto& record = found->second;
  if (record.durable_recovery_required) {
    guard.unlock();
    return DmlUpdateConsumeRecoveredDurableV1(
        context, descriptor_ref, structural_occurrence_id);
  }
  if (record.lifecycle == DmlUpdateDescriptorLifecycleV1::kCompleted) {
    result.ok = true;
    result.immutable_replay = true;
    result.prior_result = record.completed_result;
    result.canonical_result_bytes = record.canonical_result_bytes;
    result.diagnostic = MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    return result;
  }
  if (record.lifecycle != DmlUpdateDescriptorLifecycleV1::kLive) {
    result.diagnostic = DmlUpdateDescriptorDiagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.descriptor_not_live");
    return result;
  }
  EngineApiDiagnostic carrier_diagnostic;
  if (!DmlUpdateRevalidateCanonicalAuthority(
          context, record, &carrier_diagnostic)) {
    result.diagnostic = carrier_diagnostic;
    return result;
  }
  EngineApiDiagnostic availability_diagnostic;
  if (!DmlUpdateRevalidateExecutorAvailability(
          context, record, &availability_diagnostic)) {
    result.diagnostic = availability_diagnostic.code.empty()
                            ? DmlUpdateDescriptorDiagnostic(
                                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                                  "sblr.dml_update_rows.executor_evidence_missing")
                            : availability_diagnostic;
    return result;
  }
  const auto loaded =
      TransactionalRelationStore(context).LoadRelationDescriptor(
          record.relation_uuid);
  if (!loaded.ok ||
      loaded.descriptor.relation_generation != record.relation_generation ||
      loaded.descriptor.descriptor_uuid.canonical !=
          record.relation_descriptor_uuid ||
      loaded.descriptor.descriptor_generation !=
          record.relation_descriptor_generation) {
    result.diagnostic = loaded.ok
                            ? DmlUpdateDescriptorDiagnostic(
                                  "MGA.TRANSACTION.STALE",
                                  "sblr.dml_update_rows.relation_generation_stale")
                            : loaded.diagnostic;
    return result;
  }
  const auto current_column = [&](const DmlUpdateBoundColumnV1& identity)
      -> const MgaRelationColumnStorageDescriptor* {
    const auto match = std::find_if(
        loaded.descriptor.columns.begin(), loaded.descriptor.columns.end(),
        [&](const auto& column) {
          return column.column_uuid.canonical == identity.column_uuid;
        });
    return match == loaded.descriptor.columns.end() ? nullptr : &*match;
  };
  for (const auto& identity : record.assignment_columns) {
    const auto* column = current_column(identity);
    if (column == nullptr ||
        !DmlUpdateRevalidateColumn(context, *column, identity)) {
      result.diagnostic = DmlUpdateDescriptorDiagnostic(
          "MGA.TRANSACTION.STALE",
          "sblr.dml_update_rows.assignment_identity_stale");
      return result;
    }
  }
  if (record.predicate_column.has_value()) {
    const auto* column = current_column(*record.predicate_column);
    if (column == nullptr ||
        !DmlUpdateRevalidateColumn(context, *column,
                                   *record.predicate_column)) {
      result.diagnostic = DmlUpdateDescriptorDiagnostic(
          "MGA.TRANSACTION.STALE",
          "sblr.dml_update_rows.predicate_identity_stale");
      return result;
    }
  }
  if (DmlUpdateCancellationRequested(context)) {
    auto aborted_record = record;
    aborted_record.lifecycle = DmlUpdateDescriptorLifecycleV1::kFailed;
    aborted_record.completed_result = {};
    aborted_record.canonical_result_bytes.clear();
    DmlUpdatePreparedJournalAppendV1 aborted_append;
    if (!DmlUpdatePrepareJournalRecordV1(
            context, aborted_record, &aborted_append) ||
        !DmlUpdateCommitPreparedJournalRecordV1(
            context, &record, aborted_append)) {
      result.diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.cancelled_registry_publish_failed");
      return result;
    }
    DmlUpdateCopyCommittedJournalPositionV1(record, &aborted_record);
    record = std::move(aborted_record);
    result.diagnostic = DmlUpdateDescriptorDiagnostic(
        "PROCESS.CANCELLED",
        "sblr.dml_update_rows.cancelled_after_revalidation");
    return result;
  }
  record.lifecycle = DmlUpdateDescriptorLifecycleV1::kExecuting;
  result.ok = true;
  result.request = record.prepared_request;
  result.request.context = context;
  result.diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return result;
}

EngineDmlUpdateRowsCompletionResultV1 CompleteDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    const EngineUpdateRowsResult& result) {
  EngineDmlUpdateRowsCompletionResultV1 completion;
  std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
  const auto found =
      g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
  if (found == g_dml_update_descriptors.end() ||
      found->second.descriptor_ref.descriptor_generation !=
          descriptor_ref.descriptor_generation ||
      !DmlUpdateContextMatches(context, found->second) ||
      found->second.lifecycle != DmlUpdateDescriptorLifecycleV1::kExecuting) {
    completion.diagnostic = DmlUpdateDescriptorDiagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.completion_stale");
    return completion;
  }
  auto& record = found->second;
  std::vector<std::uint8_t> canonical_result_bytes;
  if (result.ok) {
    canonical_result_bytes = EncodeDmlUpdateRowsResultV1(record, result);
    if (canonical_result_bytes.size() != 256) {
      completion.diagnostic = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.result_encoding_failed");
      return completion;
    }
  }
  record.completed_result = result;
  record.canonical_result_bytes = canonical_result_bytes;
  record.lifecycle = result.ok ? DmlUpdateDescriptorLifecycleV1::kPrepared
                               : DmlUpdateDescriptorLifecycleV1::kFailed;
  completion.ok = true;
  completion.canonical_result_bytes = std::move(canonical_result_bytes);
  completion.diagnostic = MakeEngineApiDiagnostic(
      "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return completion;
}

EngineDmlUpdateRowsExecuteResultV1 ExecuteDmlUpdateRowsDescriptorV1(
    const EngineRequestContext& context,
    const EngineDmlUpdateRowsDescriptorRefV1& descriptor_ref,
    std::uint64_t structural_occurrence_id) {
  EngineDmlUpdateRowsExecuteResultV1 execution;
  const auto failure_result = [&](EngineApiDiagnostic diagnostic) {
    EngineDmlUpdateRowsExecuteResultV1 failure;
    failure.diagnostic = diagnostic;
    failure.update_result = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        context, "dml.update_rows", std::move(diagnostic));
    return failure;
  };
  const auto apply_statement_authority =
      [&](const MgaDmlUpdateStatementSavepointAuthorityV1& authority) {
        std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
        const auto found =
            g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
        return found != g_dml_update_descriptors.end() &&
               found->second.descriptor_ref.descriptor_generation ==
                   descriptor_ref.descriptor_generation &&
               DmlUpdateContextMatches(context, found->second) &&
               DmlUpdateApplyStatementMgaAuthority(authority, &found->second);
      };
  const auto mark_failed = [&]() -> bool {
    std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
    const auto found =
        g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
    if (found == g_dml_update_descriptors.end() ||
        found->second.descriptor_ref.descriptor_generation !=
            descriptor_ref.descriptor_generation ||
        !DmlUpdateContextMatches(context, found->second)) {
      return false;
    }
    auto& record = found->second;
    if (record.lifecycle == DmlUpdateDescriptorLifecycleV1::kExecuting ||
        record.lifecycle == DmlUpdateDescriptorLifecycleV1::kPrepared) {
      auto aborted_record = record;
      aborted_record.lifecycle = DmlUpdateDescriptorLifecycleV1::kFailed;
      aborted_record.completed_result = {};
      aborted_record.canonical_result_bytes.clear();
      DmlUpdatePreparedJournalAppendV1 aborted_append;
      if (!DmlUpdatePrepareJournalRecordV1(
              context, aborted_record, &aborted_append) ||
          !DmlUpdateCommitPreparedJournalRecordV1(
              context, &record, aborted_append)) {
        return false;
      }
      DmlUpdateCopyCommittedJournalPositionV1(record, &aborted_record);
      record = std::move(aborted_record);
    }
    return record.lifecycle == DmlUpdateDescriptorLifecycleV1::kFailed;
  };

  const auto consumed = ConsumeDmlUpdateRowsDescriptorV1(
      context, descriptor_ref, structural_occurrence_id);
  if (!consumed.ok) return failure_result(consumed.diagnostic);
  if (consumed.immutable_replay) {
    execution.ok = true;
    execution.immutable_replay = true;
    execution.update_result = consumed.prior_result;
    execution.canonical_result_bytes = consumed.canonical_result_bytes;
    execution.diagnostic = MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    return execution;
  }

  if (DmlUpdateCancellationRequested(context)) {
    if (!mark_failed()) {
      return failure_result(DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.cancelled_abort_publish_failed"));
    }
    return failure_result(DmlUpdateDescriptorDiagnostic(
        "PROCESS.CANCELLED",
        "sblr.dml_update_rows.cancelled_before_statement_savepoint"));
  }
  EngineDmlUpdateStatementMgaAuthorityOpenRequestV1 statement_mga_request;
  bool statement_mga_state_current = false;
  {
    std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
    const auto found =
        g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
    if (found != g_dml_update_descriptors.end() &&
        found->second.descriptor_ref.descriptor_generation ==
            descriptor_ref.descriptor_generation &&
        found->second.lifecycle == DmlUpdateDescriptorLifecycleV1::kExecuting) {
      statement_mga_request =
          DmlUpdateCurrentStatementMgaAuthorityRequest(context, found->second);
      statement_mga_state_current = true;
    }
  }
  if (!statement_mga_state_current) {
    if (!mark_failed()) {
      return failure_result(DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.stale_abort_publish_failed"));
    }
    return failure_result(DmlUpdateDescriptorDiagnostic(
        "MGA.TRANSACTION.STALE",
        "sblr.dml_update_rows.statement_savepoint_state_stale"));
  }
  const auto opened_statement_mga =
      OpenDmlUpdateStatementMgaAuthorityV1(statement_mga_request);
  if (!opened_statement_mga.ok) {
    if (!mark_failed()) {
      return failure_result(DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.open_failure_abort_publish_failed"));
    }
    return failure_result(opened_statement_mga.diagnostic);
  }
  bool intent_published = false;
  {
    std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
    const auto found =
        g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
    if (found != g_dml_update_descriptors.end() &&
        found->second.descriptor_ref.descriptor_generation ==
            descriptor_ref.descriptor_generation &&
        found->second.lifecycle == DmlUpdateDescriptorLifecycleV1::kExecuting) {
      intent_published = DmlUpdateApplyStatementMgaAuthority(
                             opened_statement_mga.authority, &found->second) &&
                         DmlUpdatePublishJournalRecordV1(context,
                                                         found->second);
    }
  }
  if (!intent_published) {
    EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1 rollback_request;
    rollback_request.current = statement_mga_request;
    rollback_request.admitted = opened_statement_mga.authority;
    const auto rolled_back =
        RollbackDmlUpdateStatementMgaAuthorityV1(rollback_request);
    const bool rollback_applied =
        rolled_back.ok && apply_statement_authority(rolled_back.authority);
    const bool abort_published = rollback_applied && mark_failed();
    return failure_result(DmlUpdateDescriptorDiagnostic(
        !rolled_back.ok ? "MGA.TRANSACTION.ROLLBACK_FAILED"
                        : "DML.UPDATE_FAILED",
        !rolled_back.ok
            ? "sblr.dml_update_rows.statement_savepoint_rollback_failed"
            : !abort_published
                  ? "sblr.dml_update_rows.intent_failure_abort_publish_failed"
            : "sblr.dml_update_rows.mutation_intent_publish_failed",
        rolled_back.diagnostic.detail));
  }
  if (DmlUpdateTakeTestFault(
          EngineDmlUpdateRowsTestFaultPointV1::after_durable_intent)) {
    return failure_result(DmlUpdateDescriptorDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.test_fault_after_durable_intent"));
  }

  // The provider token remains empty until the terminal published successor
  // is durably staged below.  Declaring it before the rollback closure makes
  // every later rollback-capable branch cancel that staged successor and
  // release its descriptor CAS lock before attempting an aborted append.
  DmlUpdatePreparedDurableSuccessorV1 durable_published_append;
  const auto rollback_failure = [&](EngineUpdateRowsResult update_result,
                                    EngineApiDiagnostic diagnostic) {
    if (durable_published_append.provider_prepared.valid()) {
      const auto cancelled =
          CancelPreparedDmlUpdateDurableOperationSuccessorV1(
              std::move(durable_published_append.provider_prepared));
      if (!cancelled.ok()) {
        const auto cancel_failure =
            cancelled.diagnostic.code.empty()
                ? DmlUpdateDescriptorDiagnostic(
                      "DML.UPDATE_FAILED",
                      "sblr.dml_update_rows.published_stage_cancel_failed",
                      "recovery_required")
                : cancelled.diagnostic;
        update_result.ok = false;
        if (update_result.operation_id.empty()) {
          update_result.operation_id = "dml.update_rows";
        }
        update_result.diagnostics.push_back(cancel_failure);
        update_result.evidence.push_back(
            {"dml_update_rows_recovery", "required"});
        EngineDmlUpdateRowsExecuteResultV1 failure;
        failure.update_result = std::move(update_result);
        failure.diagnostic = cancel_failure;
        return failure;
      }
    }
    EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1 rollback_request;
    rollback_request.current = statement_mga_request;
    rollback_request.admitted = opened_statement_mga.authority;
    const auto rolled_back =
        RollbackDmlUpdateStatementMgaAuthorityV1(rollback_request);
    const bool rollback_applied =
        rolled_back.ok && apply_statement_authority(rolled_back.authority);
    const bool abort_published = rollback_applied && mark_failed();
    update_result.ok = false;
    if (update_result.operation_id.empty()) {
      update_result.operation_id = "dml.update_rows";
    }
    if (update_result.diagnostics.empty()) {
      update_result.diagnostics.push_back(diagnostic);
    }
    update_result.evidence.push_back(
        {"dml_update_rows_statement_atomicity", "rolled_back"});
    update_result.evidence.push_back(
        {"dml_update_rows_statement_savepoint",
         opened_statement_mga.authority.savepoint_uuid});
    if (!rolled_back.ok) {
      update_result.diagnostics.push_back(MakeEngineApiDiagnostic(
          "MGA.TRANSACTION.ROLLBACK_FAILED",
          "sblr.dml_update_rows.statement_savepoint_rollback_failed",
          rolled_back.diagnostic.detail.empty()
              ? rolled_back.diagnostic.message_key
              : rolled_back.diagnostic.detail,
          true));
      update_result.evidence.push_back(
          {"dml_update_rows_recovery", "required"});
    } else {
      update_result.evidence.push_back(
          {"dml_update_rows_statement_effects", "none_visible"});
    }
    if (!abort_published) {
      const auto append_failure = DmlUpdateDescriptorDiagnostic(
          "DML.UPDATE_FAILED",
          "sblr.dml_update_rows.abort_publish_failed",
          "recovery_required");
      update_result.diagnostics.push_back(append_failure);
      diagnostic = append_failure;
    }
    EngineDmlUpdateRowsExecuteResultV1 failure;
    failure.update_result = std::move(update_result);
    failure.diagnostic = std::move(diagnostic);
    return failure;
  };

  if (DmlUpdateCancellationRequested(context)) {
    return rollback_failure(
        {}, DmlUpdateDescriptorDiagnostic(
                "PROCESS.CANCELLED",
                "sblr.dml_update_rows.cancelled_before_mutation"));
  }

  EngineUpdateRowsResult update_result;
  try {
    update_result = EngineUpdateRows(consumed.request);
  } catch (const std::bad_alloc&) {
    return rollback_failure(
        {}, DmlUpdateDescriptorDiagnostic(
                "RESOURCE.BUDGET_EXCEEDED",
                "sblr.dml_update_rows.execution_allocation_failed"));
  } catch (const std::exception&) {
    return rollback_failure(
        {}, DmlUpdateDescriptorDiagnostic(
                "DML.UPDATE_FAILED",
                "sblr.dml_update_rows.execution_exception"));
  } catch (...) {
    return rollback_failure(
        {}, DmlUpdateDescriptorDiagnostic(
                "DML.UPDATE_FAILED",
                "sblr.dml_update_rows.execution_unknown_exception"));
  }
  if (!update_result.ok) {
    const auto diagnostic = update_result.diagnostics.empty()
                                ? DmlUpdateDescriptorDiagnostic(
                                      "DML.UPDATE_FAILED",
                                      "sblr.dml_update_rows.execution_failed")
                                : update_result.diagnostics.front();
    return rollback_failure(std::move(update_result), diagnostic);
  }

  auto prepared = CompleteDmlUpdateRowsDescriptorV1(
      context, descriptor_ref, update_result);
  if (!prepared.ok || prepared.canonical_result_bytes.size() != 256) {
    const auto diagnostic = prepared.diagnostic.code.empty()
                                ? DmlUpdateDescriptorDiagnostic(
                                      "DML.UPDATE_FAILED",
                                      "sblr.dml_update_rows.result_encoding_failed")
                                : prepared.diagnostic;
    return rollback_failure(std::move(update_result), diagnostic);
  }
  bool prepared_outcome_published = false;
  {
    std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
    const auto found =
        g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
    if (found != g_dml_update_descriptors.end() &&
        found->second.descriptor_ref.descriptor_generation ==
            descriptor_ref.descriptor_generation &&
        found->second.lifecycle == DmlUpdateDescriptorLifecycleV1::kPrepared &&
        found->second.statement_savepoint_uuid ==
            opened_statement_mga.authority.savepoint_uuid &&
        found->second.statement_savepoint_generation ==
            opened_statement_mga.authority.savepoint_generation) {
      prepared_outcome_published =
          DmlUpdatePublishJournalRecordV1(context, found->second);
    }
  }
  if (!prepared_outcome_published) {
    return rollback_failure(
        std::move(update_result),
        DmlUpdateDescriptorDiagnostic(
            "DML.UPDATE_FAILED",
            "sblr.dml_update_rows.prepared_outcome_publish_failed"));
  }
  if (DmlUpdateTakeTestFault(
          EngineDmlUpdateRowsTestFaultPointV1::after_prepared_outcome)) {
    return failure_result(DmlUpdateDescriptorDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.test_fault_after_prepared_outcome"));
  }
  if (DmlUpdateCancellationRequested(context)) {
    return rollback_failure(
        std::move(update_result),
        DmlUpdateDescriptorDiagnostic(
            "PROCESS.CANCELLED",
            "sblr.dml_update_rows.cancelled_before_publication"));
  }

  // The terminal DUJR bytes are completed while rollback is still possible.
  // After the MGA publication barrier, the execution path may only compare-
  // and-append these exact bytes; it must not allocate, encode, or hash a new
  // result or journal record.
  DmlUpdatePreparedJournalAppendV1 prepared_published_append;
  bool published_append_prepared = false;
  std::string recovery_token_uuid;
  std::uint64_t recovery_generation = 0;
  try {
    std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
    const auto found =
        g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
    if (found != g_dml_update_descriptors.end() &&
        found->second.descriptor_ref.descriptor_generation ==
            descriptor_ref.descriptor_generation &&
        DmlUpdateContextMatches(context, found->second) &&
        found->second.lifecycle == DmlUpdateDescriptorLifecycleV1::kPrepared) {
      auto published_record = found->second;
      published_record.lifecycle = DmlUpdateDescriptorLifecycleV1::kCompleted;
      published_append_prepared =
          DmlUpdatePrepareJournalRecordV1(
              context, published_record, &prepared_published_append) &&
          DmlUpdatePrepareDurableSuccessorV1(
              context, found->second, prepared_published_append,
              &durable_published_append);
      recovery_token_uuid = DmlUpdateUuidText(
          found->second.canonical_carriers.recovery_token.recovery_token_uuid);
      recovery_generation = found->second.canonical_carriers.recovery_token
                                .recovery_generation;
    }
  } catch (const std::bad_alloc&) {
    published_append_prepared = false;
  }
  if (!published_append_prepared || recovery_token_uuid.empty() ||
      recovery_generation == 0) {
    return rollback_failure(
        std::move(update_result),
        DmlUpdateDescriptorDiagnostic(
            "DML.UPDATE_FAILED",
            "sblr.dml_update_rows.published_record_prebuild_failed"));
  }

  // Build the complete caller-visible success object while the savepoint is
  // still active. No allocation, hashing, encoding, or result copy is allowed
  // after the durable publication barrier has been crossed.
  try {
    execution.ok = true;
    execution.update_result = update_result;
    execution.canonical_result_bytes =
        std::move(prepared.canonical_result_bytes);
    execution.diagnostic = MakeEngineApiDiagnostic(
        "SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  } catch (const std::bad_alloc&) {
    return rollback_failure(
        std::move(update_result),
        DmlUpdateDescriptorDiagnostic(
            "RESOURCE.BUDGET_EXCEEDED",
            "sblr.dml_update_rows.result_publication_allocation_failed"));
  }

  // This immutable failure is also completed before the statement barrier.
  // If the row effects become known-applied but the terminal DUJR CAS cannot
  // be acknowledged, returning it requires only moves of already-owned
  // storage and exposes the exact recovery identity without formatting or
  // hashing after finality.
  EngineDmlUpdateRowsExecuteResultV1 known_applied_failure;
  try {
    auto known_applied_diagnostic = DmlUpdateDescriptorDiagnostic(
        "DML.UPDATE_FAILED",
        "sblr.dml_update_rows.known_applied_terminal_append_required",
        "known_applied_recovery_required");
    known_applied_diagnostic.fields.push_back(
        {"recovery_token_uuid", recovery_token_uuid});
    known_applied_diagnostic.fields.push_back(
        {"recovery_generation", std::to_string(recovery_generation)});
    known_applied_failure = failure_result(std::move(known_applied_diagnostic));
  } catch (const std::bad_alloc&) {
    return rollback_failure(
        std::move(update_result),
        DmlUpdateDescriptorDiagnostic(
            "RESOURCE.BUDGET_EXCEEDED",
            "sblr.dml_update_rows.recovery_outcome_allocation_failed"));
  }

  bool publication_state_stale = false;
  bool publication_fault_injected = false;
  bool publication_append_failed = false;
  bool publication_barrier_crossed = false;
  bool publication_authority_invalid = false;
  EngineApiDiagnostic release_failure;
  {
    std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
    const auto found =
        g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
    if (found == g_dml_update_descriptors.end() ||
        found->second.descriptor_ref.descriptor_generation !=
            descriptor_ref.descriptor_generation ||
        !DmlUpdateContextMatches(context, found->second) ||
        found->second.lifecycle != DmlUpdateDescriptorLifecycleV1::kPrepared) {
      publication_state_stale = true;
    }
  }
  EngineDmlUpdateStatementMgaAuthorityResultV1 released_statement_mga;
  if (!publication_state_stale) {
    EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1 release_request;
    release_request.current = statement_mga_request;
    release_request.admitted = opened_statement_mga.authority;
    released_statement_mga =
        ReleaseDmlUpdateStatementMgaAuthorityV1(release_request);
    publication_barrier_crossed =
        released_statement_mga.ok &&
        released_statement_mga.authority.lifecycle ==
            MgaDmlUpdateStatementSavepointLifecycleV1::released &&
        released_statement_mga.authority.publication_barrier_present;
    if (!publication_barrier_crossed) {
      release_failure = released_statement_mga.diagnostic.code.empty()
                            ? DmlUpdateDescriptorDiagnostic(
                                  "DML.UPDATE_FAILED",
                                  "sblr.dml_update_rows.publication_barrier_invalid")
                            : released_statement_mga.diagnostic;
    } else if (
        released_statement_mga.authority.publication_barrier_uuid !=
            opened_statement_mga.authority.publication_barrier_uuid ||
        released_statement_mga.authority.publication_barrier_generation !=
            opened_statement_mga.authority.publication_barrier_generation) {
      // The provider reports that a barrier crossed, but its identity no
      // longer equals the prebuilt DURS. Rollback is no longer legal and no
      // new diagnostic/result object may be allocated in this branch.
      publication_authority_invalid = true;
    } else {
      std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
      const auto found =
          g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
      if (found == g_dml_update_descriptors.end() ||
          found->second.descriptor_ref.descriptor_generation !=
              descriptor_ref.descriptor_generation ||
          !DmlUpdateContextMatches(context, found->second) ||
          found->second.lifecycle !=
              DmlUpdateDescriptorLifecycleV1::kPrepared ||
          !DmlUpdateApplyReleasedStatementMgaAuthorityNoAllocV1(
              released_statement_mga.authority, &found->second)) {
        publication_state_stale = true;
      } else if (DmlUpdateTakeTestFault(
                     EngineDmlUpdateRowsTestFaultPointV1::
                         after_publication_barrier)) {
        // The provider has durably published the exact reserved barrier.  A
        // replay must recover the immutable prepared DURS and complete it.
        publication_fault_injected = true;
      } else {
        if (!DmlUpdateCommitDurableSuccessorNoBuildV1(
                &found->second, std::move(durable_published_append))) {
          publication_append_failed = true;
        } else {
          found->second.lifecycle = DmlUpdateDescriptorLifecycleV1::kCompleted;
        }
      }
    }
  }
  if (execution.ok && !publication_state_stale &&
      !publication_fault_injected && !publication_append_failed &&
      !publication_authority_invalid && release_failure.code.empty()) {
    return execution;
  }
  if (publication_barrier_crossed &&
      (publication_fault_injected || publication_append_failed ||
       publication_state_stale || publication_authority_invalid)) {
    {
      std::lock_guard<std::mutex> guard(g_dml_update_descriptor_mutex);
      const auto found =
          g_dml_update_descriptors.find(descriptor_ref.descriptor_uuid);
      if (found != g_dml_update_descriptors.end() &&
          found->second.descriptor_ref.descriptor_generation ==
              descriptor_ref.descriptor_generation &&
          DmlUpdateContextMatches(context, found->second) &&
          found->second.lifecycle ==
              DmlUpdateDescriptorLifecycleV1::kPrepared) {
        found->second.durable_recovery_required = true;
      }
    }
    return std::move(known_applied_failure);
  }
  if (publication_state_stale) {
    execution = {};
    return rollback_failure(
        std::move(update_result),
        DmlUpdateDescriptorDiagnostic(
            "MGA.TRANSACTION.STALE",
            "sblr.dml_update_rows.publication_state_stale"));
  }
  if (!release_failure.code.empty()) {
    execution = {};
    update_result.diagnostics.push_back(release_failure);
  }
  return rollback_failure(
      std::move(update_result),
      DmlUpdateDescriptorDiagnostic(
          "MGA.TRANSACTION.INVALID",
          "sblr.dml_update_rows.statement_savepoint_release_failed"));
}

// SEARCH_KEY: SB_PID004_OPTIMIZED_UPDATE_DELETE_EXECUTOR_BEHAVIOR

EngineUpdateRowsResult ExecuteOptimizedUpdateRows(const EngineUpdateRowsRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", MakeInvalidRequestDiagnostic("dml.update_rows", "local_transaction_id_required"));
  }
  if (request.target_table.uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", MakeInvalidRequestDiagnostic("dml.update_rows", "target_table_uuid_required"));
  }
  const auto cancellation_failure = [&](std::string message_key) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        MakeEngineApiDiagnostic("PROCESS.CANCELLED",
                                std::move(message_key), {}, true));
  };
  const bool descriptor_atomic_execution =
      DmlUpdateHasTraceTag(request.context,
                           "private_dml_update_rows_binder");
  if (descriptor_atomic_execution &&
      DmlUpdateCancellationRequested(request.context)) {
    return cancellation_failure(
        "sblr.dml_update_rows.cancelled_before_target_enumeration");
  }
  const auto mutation_window_validation = ValidateMutationRowWindow(
      "dml.update_rows", request.limit, request.offset, false);
  if (mutation_window_validation.error) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        mutation_window_validation);
  }
  auto update_phase_last = UpdateDeleteSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> update_phase_micros;
  update_phase_micros.reserve(24);
  const auto mark_update_phase = [&](std::string phase) {
    const auto now = UpdateDeleteSteadyClock::now();
    update_phase_micros.push_back(
        {std::move(phase), UpdateDeleteElapsedMicros(update_phase_last, now)});
    update_phase_last = now;
  };
  const auto write_update_trace = [&](std::size_t row_count) {
    WriteUpdateDeletePhaseTrace("engine_update_rows",
                                "dml.update_rows",
                                row_count,
                                update_phase_micros);
  };
  const auto write_result_policy =
      ResolveWriteResultPolicy(request, "dml.update_rows");
  mark_update_phase("resolve_write_result_policy");
  if (!write_result_policy.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        write_result_policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  const bool suppress_payload_rows =
      WriteResultPolicySuppressesPayloadRows(write_result_policy);
  EngineUpdateRowsRequest effective_request = request;
  NormalizeUpdatePredicateFromLoweredOptions(&effective_request);
  const std::string source_uuid = UpdateOptionText(effective_request, "source_uuid:");
  const bool needs_source_scope =
      effective_request.update_predicate.predicate_kind == "column_in_projection" &&
      !source_uuid.empty();
  TransactionalRelationStore relation_store(effective_request.context);
  auto loaded = needs_source_scope
      ? relation_store.LoadMutationTargetRows(
            std::vector<std::string>{effective_request.target_table.uuid.canonical,
                                     source_uuid})
      : relation_store.LoadMutationTargetRows(
            effective_request.target_table.uuid.canonical);
  mark_update_phase("load_relation_state");
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", loaded.diagnostic); }
  if (descriptor_atomic_execution &&
      DmlUpdateCancellationRequested(request.context)) {
    return cancellation_failure(
        "sblr.dml_update_rows.cancelled_after_target_enumeration");
  }
  MgaRelationReadView state = relation_store.BuildReadView(&loaded);
  auto table = FindVisibleMgaTable(state, effective_request.target_table.uuid.canonical, effective_request.context.local_transaction_id);
  mark_update_phase("build_state_and_find_table");
  if (!table) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        MakeInvalidRequestDiagnostic("dml.update_rows", "target_table_not_visible"));
  }
  if (table->temporary && request.context.session_uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        MakeInvalidRequestDiagnostic("dml.update_rows",
                                     "temporary_table_requires_session_uuid"));
  }
  const bool executable_trigger_descriptors_present =
      dml_trigger_runtime::HasActiveTableTriggerDescriptors(
          request.context,
          request.target_table.uuid.canonical);
  const auto resolved_projection_predicate =
      ResolveColumnInProjectionPredicate(effective_request, state);
  if (resolved_projection_predicate.attempted) {
    if (!resolved_projection_predicate.ok) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
          resolved_projection_predicate.diagnostic);
    }
    effective_request.update_predicate = resolved_projection_predicate.predicate;
  }
  mark_update_phase("resolve_projection_predicate");
  if (CrudPredicateTouchesOpaqueColumn(*table, effective_request.update_predicate)) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        UnsupportedCrudFeatureDiagnostic("dml.update_rows", "opaque_column_comparison_denied"));
  }
  if (CrudAssignmentsTouchOpaqueColumn(*table, request.assignments)) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        UnsupportedCrudFeatureDiagnostic("dml.update_rows", "opaque_column_mutation_denied"));
  }
  auto serializable_admission = dml::CheckSerializablePredicateMutation(
      effective_request.context,
      "dml.update_rows",
      effective_request.target_table.uuid.canonical,
      effective_request.update_predicate,
      false,
      effective_request.option_envelopes);
  if (!serializable_admission.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        serializable_admission.diagnostic);
    failure.evidence.insert(failure.evidence.end(),
                            serializable_admission.evidence.begin(),
                            serializable_admission.evidence.end());
    return failure;
  }
  mark_update_phase("serializable_admission");

  auto visible_indexes = VisibleMgaIndexesForTable(
      state,
      effective_request.target_table.uuid.canonical,
      effective_request.context.local_transaction_id);
  MgaRelationStorageDescriptor relation_descriptor;
  const auto descriptor_ready = EnsureMgaRelationStorageDescriptor(effective_request.context, *table, visible_indexes, &relation_descriptor);
  if (descriptor_ready.error) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", descriptor_ready);
  }
  bool invalid_assignment_plan = false;
  auto assignment_expressions =
      ParseUpdateAssignmentPlan(effective_request, &invalid_assignment_plan);
  if (invalid_assignment_plan) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        effective_request.context,
        "dml.update_rows",
        MakeInvalidRequestDiagnostic("dml.update_rows", "assignment_plan_invalid"));
  }
  const auto assigned_columns =
      UpdateAssignedColumns(effective_request, assignment_expressions);
  UpdateBatchContext batch_context =
      BuildUpdateBatchContext(effective_request, state, *table, visible_indexes);
  const bool update_needs_index_entries =
      batch_context.index_plan.has_affected_unique_exact ||
      UpdateCandidateStreamNeedsIndexEntries(effective_request, visible_indexes);
  if (update_needs_index_entries && state.index_entries.empty()) {
    auto reloaded = needs_source_scope
        ? relation_store.LoadMutationTargets(
              std::vector<std::string>{request.target_table.uuid.canonical,
                                       source_uuid})
        : relation_store.LoadMutationTarget(
              request.target_table.uuid.canonical);
    if (!reloaded.ok) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
          reloaded.diagnostic);
    }
    loaded.evidence.insert(loaded.evidence.end(),
                           reloaded.evidence.begin(),
                           reloaded.evidence.end());
    state = relation_store.BuildReadView(&reloaded);
    table = FindVisibleMgaTable(state,
                                 request.target_table.uuid.canonical,
                                 request.context.local_transaction_id);
    if (!table) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
          MakeInvalidRequestDiagnostic("dml.update_rows", "target_table_not_visible"));
    }
    visible_indexes = VisibleMgaIndexesForTable(
        state,
        effective_request.target_table.uuid.canonical,
        effective_request.context.local_transaction_id);
    batch_context =
        BuildUpdateBatchContext(effective_request, state, *table, visible_indexes);
  }
  if (!batch_context.accepted) {
    RecordUpdateBatchMetric(batch_context,
                            "sb_dml_update_batch_fallback_total",
                            1.0,
                            "fallback",
                            batch_context.fallback_reason.empty() ? "update_batch_refused" : batch_context.fallback_reason);
  }
  ConstraintDmlValidationOptions update_constraint_options;
  update_constraint_options.validate_unique_constraints =
      batch_context.index_plan.has_affected_unique_exact;
  const bool validate_domain_rules =
      UpdateTouchesDomainColumns(*table, assigned_columns);
  const bool validate_row_constraints =
      UpdateTouchesImmediateConstraintColumns(*table,
                                             assigned_columns,
                                             update_constraint_options);
  const bool validate_parent_key_update =
      UpdateTouchesParentKeyColumns(*table, assigned_columns);
  mark_update_phase("descriptor_and_batch_context");

  auto result = MakeCrudSuccessResult<EngineUpdateRowsResult>(request.context, "dml.update_rows");
  result.evidence.insert(result.evidence.end(),
                         loaded.evidence.begin(),
                         loaded.evidence.end());
  result.evidence.push_back({"update_predicate_kind",
                             effective_request.update_predicate.predicate_kind});
  result.evidence.push_back({"update_predicate_column",
                             effective_request.update_predicate.canonical_predicate_envelope});
  result.evidence.push_back({"update_predicate_bound_count",
                             std::to_string(effective_request.update_predicate.bound_values.size())});
  if (batch_context.page_reservation.reservation_available) {
    ++result.dml_summary.page_reservations;
  }
  result.evidence.insert(result.evidence.end(),
                         serializable_admission.evidence.begin(),
                         serializable_admission.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         resolved_projection_predicate.evidence.begin(),
                         resolved_projection_predicate.evidence.end());
  const bool hot_update_shape_enabled = HotUpdateShapeEnabled(effective_request);
  HotUpdateIndexDisciplineCounters hot_update_counters;
  const auto hot_plus_inventory =
      storage_db::LoadLocalTransactionInventoryFromDatabase(request.context.database_path);
  if (!hot_plus_inventory.ok()) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        DiagnosticFromMgaRecord(hot_plus_inventory.diagnostic,
                                "SB-MGA-HOT-STABLE-HEAD-TXN-INV-LOAD-FAILED",
                                "row_version.hot_stable_head.inventory_load_failed"));
  }
  mark_update_phase("load_hot_inventory");
  AddMutationOptimizerEvidence("update", request.context.local_transaction_id != 0, true, &result.evidence);
  auto candidate_stream = BuildUpdateTargetCandidateStream(effective_request,
                                                           state,
                                                           *table,
                                                           visible_indexes);
  mark_update_phase("build_candidate_stream");
  result.evidence.insert(result.evidence.end(),
                         candidate_stream.evidence.begin(),
                         candidate_stream.evidence.end());
  if (IsIndexTargetAccess(candidate_stream.plan.access_kind)) {
    ++result.dml_summary.index_probes;
  }
  AddDmlSummaryFallbacksFromEvidence(candidate_stream.evidence,
                                     "update_target_access_fallback",
                                     &result.dml_summary);
  if (candidate_stream.fail_closed) {
    auto failure = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
        "dml.update_rows",
        candidate_stream.diagnostic);
    failure.evidence.insert(failure.evidence.end(),
                            result.evidence.begin(),
                            result.evidence.end());
    return failure;
  }
  std::vector<CrudRowVersionRecord> materialized_rows;
  std::vector<const CrudRowVersionRecord*> row_refs;
  if (UpdateMutationWindowActive(effective_request)) {
    materialized_rows = VisibleMgaRowsForContext(
        state,
        effective_request.target_table.uuid.canonical,
        effective_request.context);
    row_refs.reserve(materialized_rows.size());
    for (const auto& row : materialized_rows) {
      row_refs.push_back(&row);
    }
    result.evidence.push_back({"mutation_row_window", "true"});
    result.evidence.push_back({"mutation_row_window_limit",
                               std::to_string(effective_request.limit)});
    result.evidence.push_back({"mutation_row_window_offset",
                               std::to_string(effective_request.offset)});
    result.evidence.push_back({"mutation_row_window_order",
                               "mga_visible_row_uuid_ascending"});
    result.evidence.push_back({"mutation_row_window_qualification",
                               "predicate_then_offset_limit"});
  } else if (candidate_stream.rows_ready) {
    row_refs.reserve(candidate_stream.rows.size());
    for (const auto& row : candidate_stream.rows) {
      row_refs.push_back(&row);
    }
  } else if (candidate_stream.plan.access_kind ==
                 DmlTargetAccessKind::table_scan &&
             AppendOnlyUpdateCandidateRefs(
                 state,
                 effective_request.target_table.uuid.canonical,
                 effective_request.context,
                 &row_refs)) {
    result.evidence.push_back({"update_visible_row_stream",
                               "append_only_single_version_fast_path"});
  } else if (suppress_payload_rows &&
             candidate_stream.plan.access_kind ==
                 DmlTargetAccessKind::table_scan &&
             VisibleCrudRowRefsForContext(
                 state,
                 effective_request.target_table.uuid.canonical,
                 effective_request.context,
                 &row_refs)) {
    result.evidence.push_back({"update_visible_row_stream",
                               "mga_latest_ref_fast_path"});
  } else {
    materialized_rows = VisibleMgaRowsForContext(
        state,
        effective_request.target_table.uuid.canonical,
        effective_request.context);
    row_refs.reserve(materialized_rows.size());
    for (const auto& row : materialized_rows) {
      row_refs.push_back(&row);
    }
  }
  mark_update_phase("materialize_visible_rows");
  result.dml_summary.visible_rows_scanned =
      static_cast<EngineApiU64>(row_refs.size());
  const bool compact_update_row_evidence =
      suppress_payload_rows && row_refs.size() >= 1024;
  UpdateRowEvidenceCompactor row_evidence_compactor;
  row_evidence_compactor.enabled = compact_update_row_evidence;
  row_evidence_compactor.input_row_count =
      static_cast<EngineApiU64>(row_refs.size());
  EngineApiU64 compacted_match_traces = 0;
  EngineApiU64 compacted_hot_proof_traces = 0;
  EngineApiU64 compacted_write_traces = 0;
  std::unordered_map<std::string, EngineApiU64> compacted_hot_decisions;
  ConstraintDmlValidationCache constraint_cache;
  std::vector<CrudRowVersionRecord> returning_rows;
  if (!suppress_payload_rows) {
    returning_rows.reserve(row_refs.size());
  }
  std::vector<StagedUpdateRow> staged_update_rows;
  staged_update_rows.reserve(row_refs.size());
  EngineApiU64 no_effect_count = 0;
  auto prepared_update_predicate =
      PrepareUpdatePredicate(effective_request.update_predicate);
  EngineApiU64 mutation_window_qualified_rows_seen = 0;
  EngineApiU64 mutation_window_skipped_rows = 0;
  for (const auto* row_ptr : row_refs) {
    if (descriptor_atomic_execution &&
        DmlUpdateCancellationRequested(request.context)) {
      return cancellation_failure(
          "sblr.dml_update_rows.cancelled_before_candidate_batch");
    }
    if (row_ptr == nullptr) { continue; }
    const auto& row = *row_ptr;
    if (!CrudRowMatchesPreparedUpdatePredicate(
            row,
            effective_request.update_predicate,
            &prepared_update_predicate)) {
      continue;
    }
    if (UpdateMutationWindowActive(effective_request)) {
      if (mutation_window_qualified_rows_seen < effective_request.offset) {
        ++mutation_window_qualified_rows_seen;
        ++mutation_window_skipped_rows;
        continue;
      }
      ++mutation_window_qualified_rows_seen;
    }
    ++result.matched_count;
    ++batch_context.actual_match_count;
    if (compact_update_row_evidence) {
      ++compacted_match_traces;
    } else {
      AddUpdateTrace(&batch_context, "update.row.match", "match", row.row_uuid);
    }

    auto values = row.values;
    if (!assignment_expressions.empty()) {
      const auto applied = ApplyUpdateAssignmentExpressions(&assignment_expressions, &values);
      if (applied.error) {
        return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", applied);
      }
    } else {
      std::size_t assignment_target_index = std::string::npos;
      for (const auto& [field, typed] : request.assignments) {
        std::string* existing_value =
            CachedMutableCrudFieldValuePtr(&values, field, &assignment_target_index);
        if (existing_value != nullptr) {
          *existing_value = typed.is_null ? "<NULL>" : typed.encoded_value;
        } else {
          values.push_back({field, typed.is_null ? "<NULL>" : typed.encoded_value});
          assignment_target_index = values.size() - 1;
        }
      }
    }

    if (validate_domain_rules) {
      const auto domain_validation = ApplyDomainRulesToCrudValues(request.context,
                                                                  table->columns,
                                                                  values,
                                                                  request.context.local_transaction_id,
                                                                  &constraint_cache);
      if (!domain_validation.ok) {
        return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", domain_validation.diagnostic);
      }
      values = domain_validation.values;
      row_evidence_compactor.AppendOrCompact(domain_validation.evidence, &result.evidence);
    }
    if (validate_row_constraints) {
      const auto constraint_validation =
          ValidateImmediateRowConstraintsWithOptions(request.context,
                                                     state,
                                                     *table,
                                                     row.row_uuid,
                                                     values,
                                                     "update",
                                                     update_constraint_options,
                                                     &constraint_cache);
      if (!constraint_validation.ok) {
        return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", constraint_validation.diagnostic);
      }
      values = constraint_validation.values;
      row_evidence_compactor.AppendOrCompact(constraint_validation.evidence, &result.evidence);
    }
    if (validate_parent_key_update) {
      const auto parent_key_update = ValidateImmediateParentKeyUpdateConstraints(request.context,
                                                                                state,
                                                                                *table,
                                                                                row,
                                                                                values);
      if (parent_key_update.error) {
        return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", parent_key_update);
      }
    }

    // The exact ordinary descriptor profile does not manufacture a new MGA
    // row version when the fully validated final image is byte-for-byte the
    // visible prior image. Active executable triggers intentionally remain on
    // the effectful path because their ordered execution can itself produce
    // a statement-visible effect even when the scalar assignments are equal.
    if (!executable_trigger_descriptors_present && values == row.values) {
      ++no_effect_count;
      continue;
    }

    const auto encoded_bytes = EncodedValueBytes(values);
    const bool update_toast_required = encoded_bytes > kCrudVerticalSliceMaxEncodedValueBytes;
    const auto memory_validation = ValidateUpdateBatchMemoryBudget(
        batch_context,
        static_cast<EngineApiU64>(update_toast_required ? kCrudVerticalSliceMaxEncodedValueBytes : encoded_bytes));
    if (memory_validation.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", memory_validation);
    }
    const auto batch_unique = ValidateUpdateBatchUniquePreflight(&batch_context, values, row.row_uuid);
    if (batch_unique.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", batch_unique);
    }
    if (batch_context.index_plan.has_affected_unique_exact) {
      const auto unique_check =
          ValidateMgaUniqueIndexesForRow(state,
                                          request.target_table.uuid.canonical,
                                          row.row_uuid,
                                          values,
                                          request.context);
      if (unique_check.error) {
        return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
            request.context,
            "dml.update_rows",
            unique_check);
      }
    }

    const std::string version_uuid = GenerateCrudEngineUuid("row");
    CrudRowVersionRecord row_record;
    row_record.creator_tx = request.context.local_transaction_id;
    row_record.table_uuid = request.target_table.uuid.canonical;
    row_record.row_uuid = row.row_uuid;
    row_record.version_uuid = version_uuid;
    row_record.temporary_session_uuid =
        table->temporary ? request.context.session_uuid.canonical : "";
    row_record.previous_version_uuid = row.version_uuid;
    row_record.previous_sequence = row.sequence;
    row_record.deleted = false;
    row_record.values = std::move(values);
    auto index_key_states =
        BuildStagedUpdateIndexKeyStates(batch_context, row, row_record.values);
    auto hot_plus_decision = BuildHotPlusDecisionForStagedUpdate(
        request,
        hot_plus_inventory.inventory,
        batch_context,
        row,
        row_record,
        row_record.values,
        &index_key_states,
        encoded_bytes,
        update_toast_required,
        hot_update_shape_enabled);
    if (!hot_plus_decision.ok) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
          hot_plus_decision.diagnostic);
    }
    RecordHotPlusDecisionCounter(hot_plus_decision.decision,
                                 &hot_update_counters);
    const std::uint64_t unaffected_avoided =
        CountUnaffectedExactIndexChurnAvoided(batch_context,
                                              row,
                                              row_record.values,
                                              hot_plus_decision.decision,
                                              &index_key_states);
    hot_update_counters.exact_secondary_churn_avoided += unaffected_avoided;
    hot_update_counters.index_churn_avoided += unaffected_avoided;
    const std::string hot_plus_decision_name =
        mga::HotStableRowHeadDecisionName(hot_plus_decision.decision.decision);
    row_evidence_compactor.PushOrCompact(
        {"hot_plus_decision", hot_plus_decision_name},
        &result.evidence);
    if (compact_update_row_evidence) {
      ++compacted_hot_decisions[hot_plus_decision_name];
    }
    if (!hot_plus_decision.decision.ok()) {
      auto failure = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
          DiagnosticFromMgaRecord(
              hot_plus_decision.decision.diagnostic,
              "SB-MGA-HOT-STABLE-HEAD-PROOF-REFUSED",
              "row_version.hot_stable_head.proof_refused"));
      failure.evidence.insert(failure.evidence.end(),
                              result.evidence.begin(),
                              result.evidence.end());
      failure.evidence.push_back(
          {"hot_plus_decision",
           mga::HotStableRowHeadDecisionName(hot_plus_decision.decision.decision)});
      AddHotUpdateIndexDisciplineEvidence(hot_update_shape_enabled,
                                          hot_update_counters,
                                          &failure);
      AddUpdateBatchEvidenceToResult(batch_context, &failure);
      return failure;
    }
    if (compact_update_row_evidence) {
      ++compacted_hot_proof_traces;
    } else {
      AddUpdateTrace(&batch_context,
                     "update.hot_plus.decision",
                     "proof",
                     hot_plus_decision_name);
    }
    const bool retain_stage_logical_values =
        !suppress_payload_rows || executable_trigger_descriptors_present ||
        update_toast_required ||
        UpdatePlanHasMaintainableIndexWork(batch_context);
    std::vector<std::pair<std::string, std::string>> stage_logical_values;
    if (retain_stage_logical_values) {
      stage_logical_values = row_record.values;
    }
    staged_update_rows.push_back({std::move(row_record),
                                  row,
                                  std::move(stage_logical_values),
                                  std::move(hot_plus_decision.decision),
                                  std::move(index_key_states),
                                  encoded_bytes,
                                  update_toast_required});
    if (UpdateMutationWindowActive(effective_request) &&
        static_cast<EngineApiU64>(staged_update_rows.size()) >=
            effective_request.limit) {
      break;
    }
  }
  if (UpdateMutationWindowActive(effective_request)) {
    result.evidence.push_back({"mutation_row_window_qualified_rows_seen",
                               std::to_string(mutation_window_qualified_rows_seen)});
    result.evidence.push_back({"mutation_row_window_skipped_rows",
                               std::to_string(mutation_window_skipped_rows)});
    result.evidence.push_back({"mutation_row_window_applied_rows",
                               std::to_string(staged_update_rows.size())});
  }
  if (no_effect_count != 0) {
    result.evidence.push_back(
        {"dml_update_rows_no_effect_count", std::to_string(no_effect_count)});
  }
  mark_update_phase("stage_update_rows");

  if (!staged_update_rows.empty()) {
    if (descriptor_atomic_execution &&
        DmlUpdateCancellationRequested(request.context)) {
      return cancellation_failure(
          "sblr.dml_update_rows.cancelled_before_durable_mutation");
    }
    const auto row_allocation = ReserveDmlPageAllocationRuntime(
        request.context,
        request.option_envelopes,
        request.target_table.uuid.canonical,
        DmlPageAllocationRuntimeFamily::row_data,
        static_cast<std::uint64_t>(staged_update_rows.size()),
        "update.row_data");
    if (!row_allocation.ok()) {
      return AllocationFailureResult(request.context, row_allocation);
    }
    AddDmlPageAllocationRuntimeEvidence(row_allocation, &result);
    if (row_allocation.active) {
      ++result.dml_summary.page_reservations;
    }

    std::string index_allocation_owner_uuid;
    const auto planned_index_writes = PlannedUpdateIndexMaintenanceWrites(
        batch_context,
        staged_update_rows,
        hot_update_shape_enabled,
        &index_allocation_owner_uuid);
    const auto index_allocation =
        planned_index_writes == 0 || index_allocation_owner_uuid.empty()
            ? DmlPageAllocationRuntimeResult{}
            : ReserveDmlPageAllocationRuntime(request.context,
                                              request.option_envelopes,
                                              index_allocation_owner_uuid,
                                              DmlPageAllocationRuntimeFamily::index,
                                              planned_index_writes,
                                              "update.index");
    if (!index_allocation.ok()) {
      return AllocationFailureResult(request.context, index_allocation);
    }
    AddDmlPageAllocationRuntimeEvidence(index_allocation, &result);
    if (index_allocation.active) {
      ++result.dml_summary.page_reservations;
    }
    mark_update_phase("reserve_page_allocations");

    std::vector<CrudRowVersionRecord> row_records;
    row_records.reserve(staged_update_rows.size());
    for (auto& staged : staged_update_rows) {
      if (staged.toast_required) {
        auto storage_values = staged.logical_values;
        const auto large_value_persisted = PersistMgaLargeValuesForRow(request.context,
                                                                       request.target_table.uuid.canonical,
                                                                       staged.row_record.row_uuid,
                                                                       staged.row_record.version_uuid,
                                                                       true,
                                                                       &storage_values,
                                                                       &result.evidence);
        if (large_value_persisted.error) {
          return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", large_value_persisted);
        }
        staged.row_record.values = std::move(storage_values);
      }
      row_records.push_back(std::move(staged.row_record));
    }
    mark_update_phase("persist_large_values");

    auto hot_append_context = relation_store.OpenHotAppendContext();
    std::vector<std::uint64_t> written_event_sequences;
    auto serializable_recorded = dml::RecordSerializablePredicateMutation(
        effective_request.context,
        "dml.update_rows",
        effective_request.target_table.uuid.canonical,
        effective_request.update_predicate,
        false,
        effective_request.option_envelopes);
    if (!serializable_recorded.ok) {
      auto failure = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
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
    mark_update_phase("serializable_record");
    const auto appended = hot_append_context.AppendRowVersions(&row_records, &written_event_sequences);
    if (appended.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", appended);
    }
    const auto rows_flushed = hot_append_context.FlushRowVersions();
    if (rows_flushed.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", rows_flushed);
    }
    mark_update_phase("append_flush_rows");

    std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> delta_entries;
    for (std::size_t index = 0; index < staged_update_rows.size(); ++index) {
      const auto& row_record = row_records[index];
      if (compact_update_row_evidence) {
        ++compacted_write_traces;
      } else {
        AddUpdateTrace(&batch_context, "update.row.write", "write", row_record.row_uuid);
      }
      auto row_delta_entries = UpdateDeltaEntries(batch_context,
                                                  staged_update_rows[index].original_row,
                                                  row_record,
                                                  staged_update_rows[index].logical_values,
                                                  &staged_update_rows[index].index_key_states,
                                                  hot_update_shape_enabled,
                                                  staged_update_rows[index].hot_plus_decision,
                                                  &hot_update_counters);
      delta_entries.insert(delta_entries.end(),
                           std::make_move_iterator(row_delta_entries.begin()),
                           std::make_move_iterator(row_delta_entries.end()));
    }
    const auto delta_appended =
        relation_store.AppendSecondaryIndexDeltaLedgerEntries(
        delta_entries,
        compact_update_row_evidence ? nullptr : &result.evidence);
    if (delta_appended.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", delta_appended);
    }
    if (compact_update_row_evidence && !delta_entries.empty()) {
      result.evidence.push_back({"mga_secondary_index_delta_ledger_compacted",
                                 "true"});
      result.evidence.push_back({"mga_secondary_index_delta_ledger_entry_count",
                                 std::to_string(delta_entries.size())});
    }
    const auto index_appended = AppendSynchronousUpdateIndexEntries(request.context,
                                                                    batch_context,
                                                                    request.target_table.uuid.canonical,
                                                                    staged_update_rows,
                                                                    row_records,
                                                                    hot_update_shape_enabled,
                                                                    &hot_append_context,
                                                                    &hot_update_counters,
                                                                    &result.evidence);
    if (index_appended.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", index_appended);
    }
    const auto indexes_flushed = hot_append_context.FlushIndexEntries();
    if (indexes_flushed.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", indexes_flushed);
    }
    mark_update_phase("index_delta_and_flush");
    const auto& append_counters = hot_append_context.counters();
    result.dml_summary.append_calls += append_counters.row_range_reservations +
                                       append_counters.index_range_reservations;
    result.dml_summary.file_opens += append_counters.row_stream_opens +
                                     append_counters.index_stream_opens +
                                     append_counters.scoped_row_stream_opens +
                                     append_counters.scoped_index_stream_opens +
                                     append_counters.allocator_stream_opens;
    result.dml_summary.flushes += append_counters.row_stream_flushes +
                                  append_counters.index_stream_flushes +
                                  append_counters.scoped_row_stream_flushes +
                                  append_counters.scoped_index_stream_flushes +
                                  append_counters.allocator_stream_flushes;
    if (!delta_entries.empty()) {
      ++result.dml_summary.append_calls;
    }
    if (index_allocation.active) {
      result.evidence.push_back({"mga_index_store", "row_update"});
    }

    for (std::size_t index = 0; index < staged_update_rows.size(); ++index) {
      const auto& row_record = row_records[index];
      if (!suppress_payload_rows) {
        CrudRowVersionRecord returning_row;
        returning_row.creator_tx = request.context.local_transaction_id;
        returning_row.event_sequence = row_record.event_sequence;
        returning_row.sequence = row_record.sequence;
        returning_row.table_uuid = request.target_table.uuid.canonical;
        returning_row.row_uuid = row_record.row_uuid;
        returning_row.version_uuid = row_record.version_uuid;
        returning_row.previous_version_uuid = row_record.previous_version_uuid;
        returning_row.previous_sequence = row_record.previous_sequence;
        returning_row.deleted = false;
        returning_row.values = staged_update_rows[index].logical_values;
        returning_rows.push_back(std::move(returning_row));
      }
      ++result.updated_count;
      ++batch_context.actual_update_count;
    }
  }
  if (staged_update_rows.empty()) {
    auto serializable_recorded = dml::RecordSerializablePredicateMutation(
        request.context,
        "dml.update_rows",
        request.target_table.uuid.canonical,
        request.update_predicate,
        false,
        request.option_envelopes);
    if (!serializable_recorded.ok) {
      auto failure = MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
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
    mark_update_phase("serializable_record_empty_update");
  }

  AddUpdateTrace(&batch_context, "update.batch.finish", "finish", std::to_string(batch_context.actual_update_count));
  if (!suppress_payload_rows) {
    result.result_shape = CrudRowsToResultShape(returning_rows);
  }
  mark_update_phase("result_shape");
  row_evidence_compactor.AddSummaryEvidence(&result.evidence);
  if (compact_update_row_evidence) {
    result.evidence.push_back({"update_trace_compacted", "true"});
    result.evidence.push_back({"update_trace_compacted.update_row_match",
                               std::to_string(compacted_match_traces)});
    result.evidence.push_back({"update_trace_compacted.hot_plus_proof",
                               std::to_string(compacted_hot_proof_traces)});
    result.evidence.push_back({"update_trace_compacted.update_row_write",
                               std::to_string(compacted_write_traces)});
    for (const auto& [decision, count] : compacted_hot_decisions) {
      result.evidence.push_back({"hot_plus_decision_count." + decision,
                                 std::to_string(count)});
      if (count == 1) {
        result.evidence.push_back({"hot_plus_decision", decision});
      }
    }
  }
  mark_update_phase("update_trace_evidence");
  result.evidence.push_back({"mga_row_version", "row_update"});
  result.evidence.push_back({"audit_event", "data.dml_change"});
  result.evidence.push_back({"dml_surface_variant", "update"});
  result.evidence.push_back({"dml_result_shape", suppress_payload_rows ? "rs.dml.mutation.v1"
                                                                       : "rs.dml.returning.v1"});
  result.evidence.push_back({"domain_validation", "write_path_checked"});
  result.evidence.push_back({"relation_descriptor", relation_descriptor.descriptor_uuid.canonical});
  result.evidence.push_back({"dml_returning", "affected_rows"});
  if (descriptor_atomic_execution &&
      DmlUpdateCancellationRequested(request.context)) {
    return cancellation_failure(
        "sblr.dml_update_rows.cancelled_before_trigger_or_constraint_work");
  }
  if (executable_trigger_descriptors_present) {
    std::vector<dml_trigger_runtime::DmlTriggerUpdateRowImage> trigger_update_rows;
    trigger_update_rows.reserve(staged_update_rows.size());
    for (const auto& staged : staged_update_rows) {
      trigger_update_rows.push_back({staged.original_row.values, staged.logical_values});
    }
    const auto trigger_result =
        dml_trigger_runtime::FireAfterUpdateTableTriggers(request.context,
                                                          state,
                                                          request.target_table.uuid.canonical,
                                                          trigger_update_rows,
                                                          request.option_envelopes);
    if (!trigger_result.ok) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
          trigger_result.diagnostic);
    }
    result.evidence.insert(result.evidence.end(),
                           trigger_result.evidence.begin(),
                           trigger_result.evidence.end());
    result.evidence.push_back({"trigger_udr_hooks",
                               trigger_result.fired_count == 0
                                   ? "descriptor_checked"
                                   : "descriptor_executed"});
  } else {
    result.evidence.push_back({"trigger_udr_hooks", "descriptor_checked"});
  }
  mark_update_phase("trigger_runtime");
  AddHotUpdateIndexDisciplineEvidence(hot_update_shape_enabled, hot_update_counters, &result);
  AddUpdateBatchEvidenceToResult(batch_context, &result);
  if (!batch_context.fallback_reason.empty()) {
    AddDmlSummaryFallbackReason(&result.dml_summary, batch_context.fallback_reason);
  }
  result.dml_summary.rows_changed = result.updated_count;
  AddDmlSummaryEvidence(&result);
  ApplyWriteResultPolicy(write_result_policy, &result);
  mark_update_phase("summary_and_policy");
  RecordUpdateBatchMetric(batch_context, "sb_dml_update_batch_started_total", 1.0, "ok");
  RecordUpdateBatchMetric(batch_context, "sb_dml_update_rows_updated_total", static_cast<double>(result.updated_count), "ok");
  mark_update_phase("update_metrics");
  write_update_trace(static_cast<std::size_t>(result.updated_count));
  return result;
}

EngineDeleteRowsResult ExecuteOptimizedDeleteRows(const EngineDeleteRowsRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", MakeInvalidRequestDiagnostic("dml.delete_rows", "local_transaction_id_required"));
  }
  if (request.target_table.uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", MakeInvalidRequestDiagnostic("dml.delete_rows", "target_table_uuid_required"));
  }
  const std::string delete_surface_variant = DeleteSurfaceVariant(request);
  if (delete_surface_variant != "delete" &&
      delete_surface_variant != "batch_delete" &&
      delete_surface_variant != "erase" &&
      delete_surface_variant != "drop_series" &&
      delete_surface_variant != "cypher_delete" &&
      delete_surface_variant != "graph_delete_node" &&
      delete_surface_variant != "graph_delete_edge") {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        MakeInvalidRequestDiagnostic("dml.delete_rows",
                                     "unsupported_delete_surface_variant:" +
                                         delete_surface_variant));
  }
  const EngineApiU64 declared_batch_limit_rows = DeleteBatchLimitRows(request);
  const auto mutation_window_validation = ValidateMutationRowWindow(
      "dml.delete_rows",
      request.limit,
      request.offset,
      delete_surface_variant == "batch_delete" ||
          declared_batch_limit_rows != 0);
  if (mutation_window_validation.error) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        mutation_window_validation);
  }
  const EngineApiU64 batch_limit_rows =
      delete_surface_variant == "batch_delete" ? declared_batch_limit_rows : 0;
  if (delete_surface_variant == "batch_delete" && batch_limit_rows == 0) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        MakeInvalidRequestDiagnostic("dml.delete_rows",
                                     "batch_delete_limit_required"));
  }
  const auto write_result_policy =
      ResolveWriteResultPolicy(request, "dml.delete_rows");
  if (!write_result_policy.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        write_result_policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  const bool suppress_payload_rows =
      WriteResultPolicySuppressesPayloadRows(write_result_policy);
  EngineDeleteRowsRequest effective_request = request;
  NormalizeDeletePredicateFromLoweredOptions(&effective_request);
  TransactionalRelationStore relation_store(effective_request.context);
  auto loaded = relation_store.LoadConstraintScope(
      effective_request.target_table.uuid.canonical);
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", loaded.diagnostic); }
  MgaRelationReadView state = relation_store.BuildReadView(&loaded);
  const auto table = FindVisibleMgaTable(state, effective_request.target_table.uuid.canonical, effective_request.context.local_transaction_id);
  if (!table) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        MakeInvalidRequestDiagnostic("dml.delete_rows", "target_table_not_visible"));
  }
  if (table->temporary && request.context.session_uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        MakeInvalidRequestDiagnostic("dml.delete_rows",
                                     "temporary_table_requires_session_uuid"));
  }
  if (CrudPredicateTouchesOpaqueColumn(*table, effective_request.delete_predicate)) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        UnsupportedCrudFeatureDiagnostic("dml.delete_rows", "opaque_column_comparison_denied"));
  }
  auto serializable_admission = dml::CheckSerializablePredicateMutation(
      effective_request.context,
      "dml.delete_rows",
      effective_request.target_table.uuid.canonical,
      effective_request.delete_predicate,
      true,
      effective_request.option_envelopes);
  if (!serializable_admission.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        serializable_admission.diagnostic);
    failure.evidence.insert(failure.evidence.end(),
                            serializable_admission.evidence.begin(),
                            serializable_admission.evidence.end());
    return failure;
  }

  const auto visible_indexes = VisibleMgaIndexesForTable(state, effective_request.target_table.uuid.canonical, effective_request.context.local_transaction_id);
  MgaRelationStorageDescriptor relation_descriptor;
  const auto descriptor_ready = EnsureMgaRelationStorageDescriptor(request.context, *table, visible_indexes, &relation_descriptor);
  if (descriptor_ready.error) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", descriptor_ready);
  }
  DeleteBatchContext batch_context = BuildDeleteBatchContext(effective_request, state, *table, visible_indexes);
  if (!batch_context.accepted) {
    RecordDeleteBatchMetric(batch_context,
                            "sb_dml_delete_batch_fallback_total",
                            1.0,
                            "fallback",
                            batch_context.fallback_reason.empty() ? "delete_batch_refused" : batch_context.fallback_reason);
  }

  auto result = MakeCrudSuccessResult<EngineDeleteRowsResult>(effective_request.context, "dml.delete_rows");
  result.evidence.insert(result.evidence.end(),
                         loaded.evidence.begin(),
                         loaded.evidence.end());
  result.evidence.push_back({"relation_state_full_loads",
                             loaded.full_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_scoped_loads",
                             loaded.scoped_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_load_reason",
                             "target_table_delete_scope"});
  result.evidence.insert(result.evidence.end(),
                         serializable_admission.evidence.begin(),
                         serializable_admission.evidence.end());
  result.evidence.push_back({"dml_surface_variant", delete_surface_variant});
  result.evidence.push_back({"audit_event", "data.dml_change"});
  result.evidence.push_back({"dml_result_shape", suppress_payload_rows ? "rs.dml.mutation.v1"
                                                                       : "rs.dml.returning.v1"});
  if (DeleteMutationWindowActive(effective_request)) {
    result.evidence.push_back({"mutation_row_window", "true"});
    result.evidence.push_back({"mutation_row_window_limit",
                               std::to_string(effective_request.limit)});
    result.evidence.push_back({"mutation_row_window_offset",
                               std::to_string(effective_request.offset)});
    result.evidence.push_back({"mutation_row_window_order",
                               "mga_visible_row_uuid_ascending"});
    result.evidence.push_back({"mutation_row_window_qualification",
                               "predicate_then_offset_limit"});
  }
  if (delete_surface_variant == "batch_delete") {
    result.evidence.push_back({"delete_chunked_limit_applied", "true"});
    result.evidence.push_back({"delete_batch_limit_rows", std::to_string(batch_limit_rows)});
    if (!request.batch_on_column.empty()) {
      result.evidence.push_back({"delete_batch_on_column", request.batch_on_column});
    } else {
      const auto option_batch_on = DeleteOptionValue(request, "batch_on_column:");
      if (!option_batch_on.empty()) {
        result.evidence.push_back({"delete_batch_on_column", option_batch_on});
      }
    }
  } else if (delete_surface_variant == "erase") {
    result.evidence.push_back({"erase_semantics", "audit_safe_valid_time_close"});
  } else if (delete_surface_variant == "drop_series") {
    result.evidence.push_back({"drop_series_semantics", "series_tombstone"});
    if (!request.series_name.empty()) {
      result.evidence.push_back({"drop_series_name", request.series_name});
    } else {
      const auto option_series_name = DeleteOptionValue(request, "series_name:");
      if (!option_series_name.empty()) {
        result.evidence.push_back({"drop_series_name", option_series_name});
      }
    }
  }
  AddMutationOptimizerEvidence("delete", effective_request.context.local_transaction_id != 0, true, &result.evidence);
  auto candidate_stream = BuildDeleteTargetCandidateStream(effective_request,
                                                           state,
                                                           *table,
                                                           visible_indexes);
  result.evidence.insert(result.evidence.end(),
                         candidate_stream.evidence.begin(),
                         candidate_stream.evidence.end());
  if (IsIndexTargetAccess(candidate_stream.plan.access_kind)) {
    ++result.dml_summary.index_probes;
  }
  AddDmlSummaryFallbacksFromEvidence(candidate_stream.evidence,
                                     "delete_target_access_fallback",
                                     &result.dml_summary);
  if (candidate_stream.fail_closed) {
    auto failure = MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        effective_request.context,
        "dml.delete_rows",
        candidate_stream.diagnostic);
    failure.evidence.insert(failure.evidence.end(),
                            result.evidence.begin(),
                            result.evidence.end());
    return failure;
  }
  const auto rows = candidate_stream.rows_ready
                        ? candidate_stream.rows
                        : VisibleMgaRowsForContext(state,
                                                    effective_request.target_table.uuid.canonical,
                                                    effective_request.context);
  result.dml_summary.visible_rows_scanned = static_cast<EngineApiU64>(rows.size());
  std::vector<CrudRowVersionRecord> returning_rows;
  if (!suppress_payload_rows) {
    returning_rows.reserve(rows.size());
  }
  std::vector<StagedDeleteRow> staged_delete_rows;
  staged_delete_rows.reserve(rows.size());
  EngineApiU64 mutation_window_qualified_rows_seen = 0;
  EngineApiU64 mutation_window_skipped_rows = 0;
  for (const auto& row : rows) {
    if (!MgaRowVersionVisibleToContext(state, row, effective_request.context)) { continue; }
    if (!CrudRowMatchesPredicate(row, effective_request.delete_predicate)) { continue; }
    if (DeleteMutationWindowActive(effective_request)) {
      if (mutation_window_qualified_rows_seen < effective_request.offset) {
        ++mutation_window_qualified_rows_seen;
        ++mutation_window_skipped_rows;
        continue;
      }
      ++mutation_window_qualified_rows_seen;
    }
    ++result.matched_count;
    ++batch_context.actual_match_count;
    AddDeleteTrace(&batch_context, "delete.row.match", "match", row.row_uuid);

    const auto memory_validation = ValidateDeleteBatchMemoryBudget(batch_context, static_cast<EngineApiU64>(EncodedValueBytes(row.values)));
    if (memory_validation.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(effective_request.context, "dml.delete_rows", memory_validation);
    }
    const auto constraint_validation = ValidateImmediateDeleteConstraints(effective_request.context, state, *table, row);
    if (constraint_validation.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(effective_request.context, "dml.delete_rows", constraint_validation);
    }

    AddDeleteTrace(&batch_context, "delete.row.tombstone", "write", row.row_uuid);
    CrudRowVersionRecord row_record;
    row_record.creator_tx = effective_request.context.local_transaction_id;
    row_record.table_uuid = effective_request.target_table.uuid.canonical;
    row_record.row_uuid = row.row_uuid;
    row_record.version_uuid = GenerateCrudEngineUuid("row");
    row_record.temporary_session_uuid =
        table->temporary ? effective_request.context.session_uuid.canonical : "";
    row_record.previous_version_uuid = row.version_uuid;
    row_record.previous_sequence = row.sequence;
    row_record.deleted = true;
    row_record.values = row.values;
    staged_delete_rows.push_back({std::move(row_record), row});
    if ((DeleteMutationWindowActive(effective_request) &&
         static_cast<EngineApiU64>(staged_delete_rows.size()) >=
             effective_request.limit) ||
        (batch_limit_rows != 0 &&
         static_cast<EngineApiU64>(staged_delete_rows.size()) >=
             batch_limit_rows)) {
      break;
    }
  }
  if (DeleteMutationWindowActive(effective_request)) {
    result.evidence.push_back({"mutation_row_window_qualified_rows_seen",
                               std::to_string(mutation_window_qualified_rows_seen)});
    result.evidence.push_back({"mutation_row_window_skipped_rows",
                               std::to_string(mutation_window_skipped_rows)});
    result.evidence.push_back({"mutation_row_window_applied_rows",
                               std::to_string(staged_delete_rows.size())});
  }

  if (!staged_delete_rows.empty()) {
    std::vector<CrudRowVersionRecord> row_records;
    row_records.reserve(staged_delete_rows.size());
    for (const auto& staged : staged_delete_rows) {
      row_records.push_back(staged.row_record);
    }
    std::vector<std::uint64_t> written_event_sequences;
    auto serializable_recorded = dml::RecordSerializablePredicateMutation(
        effective_request.context,
        "dml.delete_rows",
        effective_request.target_table.uuid.canonical,
        effective_request.delete_predicate,
        true,
        effective_request.option_envelopes);
    if (!serializable_recorded.ok) {
      auto failure = MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
          effective_request.context,
          "dml.delete_rows",
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
    auto hot_append_context = relation_store.OpenHotAppendContext();
    const auto appended = hot_append_context.AppendRowVersions(&row_records, &written_event_sequences);
    if (appended.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(effective_request.context, "dml.delete_rows", appended);
    }
    const auto rows_flushed = hot_append_context.FlushRowVersions();
    if (rows_flushed.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(effective_request.context, "dml.delete_rows", rows_flushed);
    }
    std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> delta_entries;
    for (std::size_t index = 0; index < staged_delete_rows.size(); ++index) {
      auto row_delta_entries = DeleteDeltaEntries(batch_context,
                                                  row_records[index],
                                                  staged_delete_rows[index].original_row);
      delta_entries.insert(delta_entries.end(),
                           std::make_move_iterator(row_delta_entries.begin()),
                           std::make_move_iterator(row_delta_entries.end()));
    }
    const auto delta_appended =
        relation_store.AppendSecondaryIndexDeltaLedgerEntries(
        delta_entries,
        &result.evidence);
    if (delta_appended.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(effective_request.context, "dml.delete_rows", delta_appended);
    }
    if (!delta_entries.empty()) {
      ++result.dml_summary.append_calls;
    }
    const auto index_retired = PrepareSynchronousDeleteIndexRetires(
        effective_request.context,
        batch_context,
        staged_delete_rows,
        row_records,
        &hot_append_context,
        &result.evidence);
    if (index_retired.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
          effective_request.context, "dml.delete_rows", index_retired);
    }
    const auto indexes_flushed = hot_append_context.FlushIndexEntries();
    if (indexes_flushed.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
          effective_request.context, "dml.delete_rows", indexes_flushed);
    }
    const auto& append_counters = hot_append_context.counters();
    result.dml_summary.append_calls += append_counters.row_range_reservations +
                                       append_counters.index_range_reservations;
    result.dml_summary.file_opens += append_counters.row_stream_opens +
                                     append_counters.index_stream_opens +
                                     append_counters.scoped_row_stream_opens +
                                     append_counters.scoped_index_stream_opens +
                                     append_counters.allocator_stream_opens;
    result.dml_summary.flushes += append_counters.row_stream_flushes +
                                  append_counters.index_stream_flushes +
                                  append_counters.scoped_row_stream_flushes +
                                  append_counters.scoped_index_stream_flushes +
                                  append_counters.allocator_stream_flushes;
    for (std::size_t index = 0; index < staged_delete_rows.size(); ++index) {
      const auto& row_record = row_records[index];
      if (!suppress_payload_rows) {
        CrudRowVersionRecord returning_row = staged_delete_rows[index].original_row;
        returning_row.creator_tx = effective_request.context.local_transaction_id;
        returning_row.event_sequence = row_record.event_sequence;
        returning_row.sequence = row_record.sequence;
        returning_row.deleted = true;
        returning_rows.push_back(std::move(returning_row));
      }
      ++result.deleted_count;
      ++batch_context.actual_delete_count;
    }
  }
  if (staged_delete_rows.empty()) {
    auto serializable_recorded = dml::RecordSerializablePredicateMutation(
        effective_request.context,
        "dml.delete_rows",
        effective_request.target_table.uuid.canonical,
        effective_request.delete_predicate,
        true,
        effective_request.option_envelopes);
    if (!serializable_recorded.ok) {
      auto failure = MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
          effective_request.context,
          "dml.delete_rows",
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
  }

  AddDeleteTrace(&batch_context, "delete.batch.finish", "finish", std::to_string(batch_context.actual_delete_count));
  if (!suppress_payload_rows) {
    result.result_shape = CrudRowsToResultShape(returning_rows);
  }
  result.evidence.push_back({"mga_row_version", "row_delete_tombstone"});
  result.evidence.push_back({"relation_descriptor", relation_descriptor.descriptor_uuid.canonical});
  result.evidence.push_back({"dml_returning", "affected_rows"});
  std::vector<CrudRowVersionRecord> trigger_delete_rows;
  trigger_delete_rows.reserve(staged_delete_rows.size());
  for (const auto& staged : staged_delete_rows) {
    trigger_delete_rows.push_back(staged.original_row);
  }
  const auto trigger_result =
      dml_trigger_runtime::FireAfterDeleteTableTriggers(effective_request.context,
                                                        state,
                                                        effective_request.target_table.uuid.canonical,
                                                        trigger_delete_rows,
                                                        effective_request.option_envelopes);
  if (!trigger_result.ok) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        effective_request.context,
        "dml.delete_rows",
        trigger_result.diagnostic);
  }
  result.evidence.insert(result.evidence.end(),
                         trigger_result.evidence.begin(),
                         trigger_result.evidence.end());
  result.evidence.push_back({"trigger_udr_hooks",
                             trigger_result.fired_count == 0
                                 ? "descriptor_checked"
                                 : "descriptor_executed"});
  AddDeleteBatchEvidenceToResult(batch_context, &result);
  if (!batch_context.fallback_reason.empty()) {
    AddDmlSummaryFallbackReason(&result.dml_summary, batch_context.fallback_reason);
  }
  result.dml_summary.rows_changed = result.deleted_count;
  AddDmlSummaryEvidence(&result);
  ApplyWriteResultPolicy(write_result_policy, &result);
  RecordDeleteBatchMetric(batch_context, "sb_dml_delete_batch_started_total", 1.0, "ok");
  RecordDeleteBatchMetric(batch_context, "sb_dml_delete_rows_deleted_total", static_cast<double>(result.deleted_count), "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api
