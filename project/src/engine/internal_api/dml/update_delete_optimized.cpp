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
#include "dml/dml_row_locator_stream.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "dml/index_apply_locality_bridge.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "dml/update_batch.hpp"
#include "dml/write_result_policy.hpp"
#include "domain_support/domain_store.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/dml_summary_counters.hpp"
#include "physical_plan.hpp"
#include "relational_planner.hpp"
#include "row_version.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
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
  bool case_ge_thresholds_integral = false;
};

bool ParseLongDoubleValue(const std::string& value, long double* out);
bool ParseLongLongValue(const std::string& value, long long* out);

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
    const std::vector<UpdateAssignmentExpression>& expressions,
    std::vector<std::pair<std::string, std::string>>* values) {
  if (values == nullptr) {
    return MakeInvalidRequestDiagnostic("dml.update_rows", "assignment_values_required");
  }
  for (const auto& expression : expressions) {
    std::string new_value = expression.literal_value;
    if (expression.operation == "copy_column") {
      new_value = CrudFieldValue(*values, expression.source_column);
    } else if (expression.operation == "concat") {
      new_value = CrudFieldValue(*values, expression.source_column) + expression.literal_value;
    } else if (expression.operation == "case_ge_thresholds") {
      bool case_ok = false;
      new_value = EvaluateCaseGeThresholds(
          CrudFieldValue(*values, expression.source_column),
          expression,
          &case_ok);
      if (!case_ok) {
        return MakeInvalidRequestDiagnostic("dml.update_rows",
                                            "assignment_case_threshold_evaluation_failed");
      }
    } else if (expression.operation == "add" ||
               expression.operation == "subtract" ||
               expression.operation == "multiply") {
      const std::string source_value = CrudFieldValue(*values, expression.source_column);
      long double left = 0.0;
      long double right = 0.0;
      if (!ParseLongDoubleValue(source_value, &left) ||
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
                                         LooksIntegralText(source_value) &&
                                             LooksIntegralText(expression.literal_value));
    }
    bool replaced = false;
    for (auto& [field, value] : *values) {
      if (field == expression.target_column) {
        value = new_value;
        replaced = true;
        break;
      }
    }
    if (!replaced) { values->push_back({expression.target_column, std::move(new_value)}); }
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
  return predicate.predicate_kind == "columns_all_not_null" ||
         predicate.predicate_kind == "column_equals_column_or_left_null" ||
         predicate.predicate_kind == "column_mod_equals" ||
         predicate.predicate_kind == "column_in_list" ||
         predicate.predicate_kind == "column_less_or_null" ||
         predicate.predicate_kind == "column_greater" ||
         predicate.predicate_kind == "column_greater_equal";
}

std::string LowerAsciiCopy(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

std::uint64_t UpdateMaxCommittedCrudTransactionId(const CrudState& state) {
  std::uint64_t max_committed = 0;
  for (const auto& [tx, status] : state.transactions) {
    if (status == "committed" || status == "archived") {
      max_committed = std::max(max_committed, tx);
    }
  }
  return max_committed;
}

std::uint64_t UpdateVisibilityHighWaterForContext(
    const CrudState& state,
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
    const CrudState& state,
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
    const CrudState& state,
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
  std::string column_name;
  double numeric_bound = 0.0;
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
  return prepared;
}

bool CrudRowMatchesPreparedUpdatePredicate(
    const CrudRowVersionRecord& row,
    const EnginePredicateEnvelope& predicate,
    const PreparedUpdatePredicate& prepared) {
  if (prepared.column_less_or_null_numeric) {
    const auto* value = CrudFieldValuePtr(row.values, prepared.column_name);
    if (value == nullptr || value->empty() || *value == "<NULL>") {
      return true;
    }
    double parsed = 0.0;
    if (TryParseFiniteDoubleNoThrow(*value, &parsed)) {
      return parsed < prepared.numeric_bound;
    }
  }
  return CrudRowMatchesPredicate(row, predicate);
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
    const CrudState& state) {
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
  const auto source_table = FindVisibleCrudTable(state,
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
  const auto source_rows = VisibleCrudRowsForContext(state,
                                                     source_uuid,
                                                     request.context);
  std::set<std::string> admitted_values;
  for (const auto& row : source_rows) {
    if (!CrudRowMatchesPredicate(row, source_predicate)) continue;
    const std::string value = CrudFieldValue(row.values, select_column);
    if (!value.empty() && value != "<NULL>") {
      admitted_values.insert(value);
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
    const CrudState& state,
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
    if (!CrudRowVersionVisibleToContext(state, row, context)) {
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
    const CrudState& state,
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
        !CrudRowVersionVisibleToContext(state, row, context)) {
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
    const CrudState& state,
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
    const CrudState& state,
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

bool HotPlusExactIndexKeysUnchanged(
    const UpdateBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const std::vector<std::pair<std::string, std::string>>& new_values) {
  for (const auto& entry : batch_context.index_plan.entries) {
    if (!IsSynchronousUpdateIndexAction(entry.action) &&
        entry.action != UpdateIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    if (IndexKeysChanged(entry.index, old_row.values, new_values)) {
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
      HotPlusExactIndexKeysUnchanged(batch_context, old_row, new_values);
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
  const auto new_row_uuid =
      ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
                        new_row.row_uuid);
  const auto old_version_uuid =
      ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
                        old_row.version_uuid);
  const auto new_previous_version_uuid =
      ParseHotProofUuid(scratchbird::core::platform::UuidKind::row,
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
    const mga::HotStableRowHeadDecisionResult& decision) {
  if (!HotPlusDecisionAvoidsExactChurn(decision)) {
    return 0;
  }
  if (!UpdatePlanHasMaintainableIndexWork(batch_context)) {
    return 0;
  }
  std::uint64_t avoided = 0;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action != UpdateIndexMaintenanceAction::unaffected) {
      continue;
    }
    if (IndexKeysChanged(entry.index, old_row.values, new_values)) {
      continue;
    }
    avoided += static_cast<std::uint64_t>(
        CrudIndexKeysForValues(entry.index, old_row.values).size());
  }
  return avoided;
}

bool ShouldMaintainUpdateIndex(const UpdateIndexMaintenancePlanEntry& entry,
                               const CrudRowVersionRecord& old_row,
                               const std::vector<std::pair<std::string, std::string>>& new_values,
                               bool hot_update_shape_enabled,
                               const mga::HotStableRowHeadDecisionResult& hot_plus_decision) {
  if (!IsSynchronousUpdateIndexAction(entry.action) &&
      entry.action != UpdateIndexMaintenanceAction::committed_delta_ledger) {
    return false;
  }
  if (!hot_update_shape_enabled) {
    return true;
  }
  const bool keys_changed =
      IndexKeysChanged(entry.index, old_row.values, new_values);
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
  for (const auto& entry : batch_context.index_plan.entries) {
    for (const auto& staged : staged_update_rows) {
      if (!ShouldMaintainUpdateIndex(entry,
                                     staged.original_row,
                                     staged.logical_values,
                                     hot_update_shape_enabled,
                                     staged.hot_plus_decision)) {
        continue;
      }
      if (first_index_uuid != nullptr && first_index_uuid->empty()) {
        *first_index_uuid = entry.index.index_uuid;
      }
      if (entry.action == UpdateIndexMaintenanceAction::committed_delta_ledger) {
        planned_writes += static_cast<std::uint64_t>(
            CrudIndexKeysForValues(entry.index, staged.original_row.values).size());
      }
      planned_writes += static_cast<std::uint64_t>(
          CrudIndexKeysForValues(entry.index, staged.logical_values).size());
    }
  }
  return planned_writes;
}

std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> UpdateDeltaEntries(
    const UpdateBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const CrudRowVersionRecord& new_row,
    const std::vector<std::pair<std::string, std::string>>& new_values,
    bool hot_update_shape_enabled,
    const mga::HotStableRowHeadDecisionResult& hot_plus_decision,
    HotUpdateIndexDisciplineCounters* counters) {
  // DPC_DEFERRED_INDEX_WRITE_PATH
  // DPC_HOT_UPDATE_SHAPE
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  if (!UpdatePlanHasMaintainableIndexWork(batch_context)) {
    return entries;
  }
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action != UpdateIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    const bool keys_changed = IndexKeysChanged(entry.index, old_row.values, new_values);
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
  std::vector<MgaIndexEntryAppendBatch> append_batches;
  if (!UpdatePlanHasMaintainableIndexWork(batch_context)) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  for (const auto& entry : batch_context.index_plan.entries) {
    if (!IsSynchronousUpdateIndexAction(entry.action)) {
      continue;
    }
    std::vector<MgaIndexEntryRowInput> index_rows;
    index_rows.reserve(staged_update_rows.size());
    for (std::size_t index = 0; index < staged_update_rows.size(); ++index) {
      const auto& staged = staged_update_rows[index];
      const bool keys_changed =
          IndexKeysChanged(entry.index, staged.original_row.values, staged.logical_values);
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
      index_rows.push_back({row_records[index].row_uuid,
                            row_records[index].version_uuid,
                            staged.logical_values});
    }
    if (!index_rows.empty()) {
      MgaIndexEntryAppendBatch batch;
      batch.index = entry.index;
      batch.table_uuid = table_uuid;
      batch.rows = std::move(index_rows);
      append_batches.push_back(std::move(batch));
    }
  }
  if (!append_batches.empty()) {
    const auto index_apply_plan =
        PlanLocalityAwareIndexApplyBatches(append_batches);
    if (index_apply_plan.diagnostic.error) {
      return index_apply_plan.diagnostic;
    }
    AddLocalityAwareIndexApplyEvidence(index_apply_plan, evidence);
    if (append_context != nullptr) {
      return append_context->AppendIndexEntryBatches(index_apply_plan.batches);
    }
    MgaRelationHotAppendContext local_append_context(context);
    const auto appended =
        local_append_context.AppendIndexEntryBatches(index_apply_plan.batches);
    if (appended.error) { return appended; }
    return local_append_context.FlushIndexEntries();
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

}  // namespace

// SEARCH_KEY: SB_PID004_OPTIMIZED_UPDATE_DELETE_EXECUTOR_BEHAVIOR

EngineUpdateRowsResult ExecuteOptimizedUpdateRows(const EngineUpdateRowsRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", MakeInvalidRequestDiagnostic("dml.update_rows", "local_transaction_id_required"));
  }
  if (request.target_table.uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", MakeInvalidRequestDiagnostic("dml.update_rows", "target_table_uuid_required"));
  }
  auto update_phase_last = UpdateDeleteSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> update_phase_micros;
  update_phase_micros.reserve(18);
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
  const std::string source_uuid = UpdateOptionText(request, "source_uuid:");
  const bool needs_source_scope =
      request.update_predicate.predicate_kind == "column_in_projection" &&
      !source_uuid.empty();
  auto loaded = needs_source_scope
      ? LoadMgaRelationStoreRowsOnlyForMutationTargets(
            request.context,
            std::vector<std::string>{request.target_table.uuid.canonical,
                                     source_uuid})
      : LoadMgaRelationStoreRowsOnlyForMutationTarget(
            request.context,
            request.target_table.uuid.canonical);
  mark_update_phase("load_relation_state");
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", loaded.diagnostic); }
  CrudState state = BuildCrudCompatibilityStateFromMga(std::move(loaded.state));
  auto table = FindVisibleCrudTable(state, request.target_table.uuid.canonical, request.context.local_transaction_id);
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
  EngineUpdateRowsRequest effective_request = request;
  const auto resolved_projection_predicate =
      ResolveColumnInProjectionPredicate(request, state);
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

  auto visible_indexes = VisibleCrudIndexesForTable(
      state,
      effective_request.target_table.uuid.canonical,
      effective_request.context.local_transaction_id);
  MgaRelationStorageDescriptor relation_descriptor;
  const auto descriptor_ready = EnsureMgaRelationStorageDescriptor(request.context, *table, visible_indexes, &relation_descriptor);
  if (descriptor_ready.error) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", descriptor_ready);
  }
  bool invalid_assignment_plan = false;
  const auto assignment_expressions =
      ParseUpdateAssignmentPlan(effective_request, &invalid_assignment_plan);
  if (invalid_assignment_plan) {
    return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
        request.context,
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
        ? LoadMgaRelationStoreStateForMutationTargets(
              request.context,
              std::vector<std::string>{request.target_table.uuid.canonical,
                                       source_uuid})
        : LoadMgaRelationStoreStateForMutationTarget(
              request.context,
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
    state = BuildCrudCompatibilityStateFromMga(std::move(reloaded.state));
    table = FindVisibleCrudTable(state,
                                 request.target_table.uuid.canonical,
                                 request.context.local_transaction_id);
    if (!table) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(
          request.context,
          "dml.update_rows",
          MakeInvalidRequestDiagnostic("dml.update_rows", "target_table_not_visible"));
    }
    visible_indexes = VisibleCrudIndexesForTable(
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
  if (candidate_stream.rows_ready) {
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
  } else {
    materialized_rows = VisibleCrudRowsForContext(
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
  const auto prepared_update_predicate =
      PrepareUpdatePredicate(effective_request.update_predicate);
  for (const auto* row_ptr : row_refs) {
    if (row_ptr == nullptr) { continue; }
    const auto& row = *row_ptr;
    if (!CrudRowMatchesPreparedUpdatePredicate(
            row,
            effective_request.update_predicate,
            prepared_update_predicate)) {
      continue;
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
      const auto applied = ApplyUpdateAssignmentExpressions(assignment_expressions, &values);
      if (applied.error) {
        return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", applied);
      }
    } else {
      for (const auto& [field, typed] : request.assignments) {
        bool replaced = false;
        for (auto& [existing_field, existing_value] : values) {
          if (existing_field == field) {
            existing_value = typed.is_null ? "<NULL>" : typed.encoded_value;
            replaced = true;
          }
        }
        if (!replaced) { values.push_back({field, typed.is_null ? "<NULL>" : typed.encoded_value}); }
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
          ValidateCrudUniqueIndexesForRow(state,
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
    auto hot_plus_decision = BuildHotPlusDecisionForStagedUpdate(
        request,
        hot_plus_inventory.inventory,
        batch_context,
        row,
        row_record,
        row_record.values,
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
                                              hot_plus_decision.decision);
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
        !suppress_payload_rows || update_toast_required ||
        UpdatePlanHasMaintainableIndexWork(batch_context);
    std::vector<std::pair<std::string, std::string>> stage_logical_values;
    if (retain_stage_logical_values) {
      stage_logical_values = row_record.values;
    }
    staged_update_rows.push_back({std::move(row_record),
                                  row,
                                  std::move(stage_logical_values),
                                  std::move(hot_plus_decision.decision),
                                  encoded_bytes,
                                  update_toast_required});
  }
  mark_update_phase("stage_update_rows");

  if (!staged_update_rows.empty()) {
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

    MgaRelationHotAppendContext hot_append_context(request.context);
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
                                                  hot_update_shape_enabled,
                                                  staged_update_rows[index].hot_plus_decision,
                                                  &hot_update_counters);
      delta_entries.insert(delta_entries.end(),
                           std::make_move_iterator(row_delta_entries.begin()),
                           std::make_move_iterator(row_delta_entries.end()));
    }
    const auto delta_appended = AppendMgaSecondaryIndexDeltaLedgerEntries(
        request.context,
        delta_entries,
        &result.evidence);
    if (delta_appended.error) {
      return MakeCrudDiagnosticResult<EngineUpdateRowsResult>(request.context, "dml.update_rows", delta_appended);
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
    }
  }
  result.evidence.push_back({"mga_row_version", "row_update"});
  result.evidence.push_back({"domain_validation", "write_path_checked"});
  result.evidence.push_back({"relation_descriptor", relation_descriptor.descriptor_uuid.canonical});
  result.evidence.push_back({"dml_returning", "affected_rows"});
  result.evidence.push_back({"trigger_udr_hooks", "inactive_checked"});
  AddHotUpdateIndexDisciplineEvidence(hot_update_shape_enabled, hot_update_counters, &result);
  AddUpdateBatchEvidenceToResult(batch_context, &result);
  if (!batch_context.fallback_reason.empty()) {
    AddDmlSummaryFallbackReason(&result.dml_summary, batch_context.fallback_reason);
  }
  result.dml_summary.rows_changed = result.updated_count;
  AddDmlSummaryEvidence(&result);
  ApplyWriteResultPolicy(write_result_policy, &result);
  RecordUpdateBatchMetric(batch_context, "sb_dml_update_batch_started_total", 1.0, "ok");
  RecordUpdateBatchMetric(batch_context, "sb_dml_update_rows_updated_total", static_cast<double>(result.updated_count), "ok");
  mark_update_phase("result_evidence");
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
  auto loaded = LoadMgaRelationStoreStateForMutationTarget(
      request.context,
      request.target_table.uuid.canonical);
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", loaded.diagnostic); }
  CrudState state = BuildCrudCompatibilityStateFromMga(std::move(loaded.state));
  const auto table = FindVisibleCrudTable(state, request.target_table.uuid.canonical, request.context.local_transaction_id);
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
  if (CrudPredicateTouchesOpaqueColumn(*table, request.delete_predicate)) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
        request.context,
        "dml.delete_rows",
        UnsupportedCrudFeatureDiagnostic("dml.delete_rows", "opaque_column_comparison_denied"));
  }
  auto serializable_admission = dml::CheckSerializablePredicateMutation(
      request.context,
      "dml.delete_rows",
      request.target_table.uuid.canonical,
      request.delete_predicate,
      true,
      request.option_envelopes);
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

  const auto visible_indexes = VisibleCrudIndexesForTable(state, request.target_table.uuid.canonical, request.context.local_transaction_id);
  MgaRelationStorageDescriptor relation_descriptor;
  const auto descriptor_ready = EnsureMgaRelationStorageDescriptor(request.context, *table, visible_indexes, &relation_descriptor);
  if (descriptor_ready.error) {
    return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", descriptor_ready);
  }
  DeleteBatchContext batch_context = BuildDeleteBatchContext(request, state, *table, visible_indexes);
  if (!batch_context.accepted) {
    RecordDeleteBatchMetric(batch_context,
                            "sb_dml_delete_batch_fallback_total",
                            1.0,
                            "fallback",
                            batch_context.fallback_reason.empty() ? "delete_batch_refused" : batch_context.fallback_reason);
  }

  auto result = MakeCrudSuccessResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows");
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
  AddMutationOptimizerEvidence("delete", request.context.local_transaction_id != 0, true, &result.evidence);
  auto candidate_stream = BuildDeleteTargetCandidateStream(request,
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
        request.context,
        "dml.delete_rows",
        candidate_stream.diagnostic);
    failure.evidence.insert(failure.evidence.end(),
                            result.evidence.begin(),
                            result.evidence.end());
    return failure;
  }
  const auto rows = candidate_stream.rows_ready
                        ? candidate_stream.rows
                        : VisibleCrudRowsForContext(state,
                                                    request.target_table.uuid.canonical,
                                                    request.context);
  result.dml_summary.visible_rows_scanned = static_cast<EngineApiU64>(rows.size());
  std::vector<CrudRowVersionRecord> returning_rows;
  if (!suppress_payload_rows) {
    returning_rows.reserve(rows.size());
  }
  std::vector<StagedDeleteRow> staged_delete_rows;
  staged_delete_rows.reserve(rows.size());
  for (const auto& row : rows) {
    if (!CrudRowVersionVisibleToContext(state, row, request.context)) { continue; }
    if (!CrudRowMatchesPredicate(row, request.delete_predicate)) { continue; }
    ++result.matched_count;
    ++batch_context.actual_match_count;
    AddDeleteTrace(&batch_context, "delete.row.match", "match", row.row_uuid);

    const auto memory_validation = ValidateDeleteBatchMemoryBudget(batch_context, static_cast<EngineApiU64>(EncodedValueBytes(row.values)));
    if (memory_validation.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", memory_validation);
    }
    const auto constraint_validation = ValidateImmediateDeleteConstraints(request.context, state, *table, row);
    if (constraint_validation.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", constraint_validation);
    }

    AddDeleteTrace(&batch_context, "delete.row.tombstone", "write", row.row_uuid);
    CrudRowVersionRecord row_record;
    row_record.creator_tx = request.context.local_transaction_id;
    row_record.table_uuid = request.target_table.uuid.canonical;
    row_record.row_uuid = row.row_uuid;
    row_record.version_uuid = GenerateCrudEngineUuid("row");
    row_record.temporary_session_uuid =
        table->temporary ? request.context.session_uuid.canonical : "";
    row_record.previous_version_uuid = row.version_uuid;
    row_record.previous_sequence = row.sequence;
    row_record.deleted = true;
    row_record.values = row.values;
    staged_delete_rows.push_back({std::move(row_record), row});
  }

  if (!staged_delete_rows.empty()) {
    std::vector<CrudRowVersionRecord> row_records;
    row_records.reserve(staged_delete_rows.size());
    for (const auto& staged : staged_delete_rows) {
      row_records.push_back(staged.row_record);
    }
    std::vector<std::uint64_t> written_event_sequences;
    auto serializable_recorded = dml::RecordSerializablePredicateMutation(
        request.context,
        "dml.delete_rows",
        request.target_table.uuid.canonical,
        request.delete_predicate,
        true,
        request.option_envelopes);
    if (!serializable_recorded.ok) {
      auto failure = MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
          request.context,
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
    MgaRelationHotAppendContext hot_append_context(request.context);
    const auto appended = hot_append_context.AppendRowVersions(&row_records, &written_event_sequences);
    if (appended.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", appended);
    }
    const auto rows_flushed = hot_append_context.FlushRowVersions();
    if (rows_flushed.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", rows_flushed);
    }
    const auto& append_counters = hot_append_context.counters();
    result.dml_summary.append_calls += append_counters.row_range_reservations;
    result.dml_summary.file_opens += append_counters.row_stream_opens +
                                     append_counters.scoped_row_stream_opens +
                                     append_counters.allocator_stream_opens;
    result.dml_summary.flushes += append_counters.row_stream_flushes +
                                  append_counters.scoped_row_stream_flushes +
                                  append_counters.allocator_stream_flushes;
    std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> delta_entries;
    for (std::size_t index = 0; index < staged_delete_rows.size(); ++index) {
      auto row_delta_entries = DeleteDeltaEntries(batch_context,
                                                  row_records[index],
                                                  staged_delete_rows[index].original_row);
      delta_entries.insert(delta_entries.end(),
                           std::make_move_iterator(row_delta_entries.begin()),
                           std::make_move_iterator(row_delta_entries.end()));
    }
    const auto delta_appended = AppendMgaSecondaryIndexDeltaLedgerEntries(
        request.context,
        delta_entries,
        &result.evidence);
    if (delta_appended.error) {
      return MakeCrudDiagnosticResult<EngineDeleteRowsResult>(request.context, "dml.delete_rows", delta_appended);
    }
    if (!delta_entries.empty()) {
      ++result.dml_summary.append_calls;
    }
    for (std::size_t index = 0; index < staged_delete_rows.size(); ++index) {
      const auto& row_record = row_records[index];
      if (!suppress_payload_rows) {
        CrudRowVersionRecord returning_row = staged_delete_rows[index].original_row;
        returning_row.creator_tx = request.context.local_transaction_id;
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
        request.context,
        "dml.delete_rows",
        request.target_table.uuid.canonical,
        request.delete_predicate,
        true,
        request.option_envelopes);
    if (!serializable_recorded.ok) {
      auto failure = MakeCrudDiagnosticResult<EngineDeleteRowsResult>(
          request.context,
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
  result.evidence.push_back({"trigger_udr_hooks", "inactive_checked"});
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
