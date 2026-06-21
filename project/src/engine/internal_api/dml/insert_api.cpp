// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/insert_api.hpp"

#include "crud_support/crud_store.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/insert_batch.hpp"
#include "dml/insert_physical_integration.hpp"
#include "dml/dml_row_locator_stream.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "dml/write_result_policy.hpp"
#include "domain_support/domain_store.hpp"
#include "ipar_fault_injection.hpp"
#include "metric_contracts.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/dml_summary_counters.hpp"
#include "physical_plan.hpp"
#include "relational_planner.hpp"
#include "security/deep_enforcement_api.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

using InsertApiSteadyClock = std::chrono::steady_clock;

std::uint64_t InsertApiElapsedMicros(InsertApiSteadyClock::time_point start,
                                     InsertApiSteadyClock::time_point finish) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void WriteInsertApiPhaseTrace(
    std::string_view layer,
    std::string_view operation_id,
    std::size_t row_count,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_INSERT_API_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "layer=" << layer
      << "\toperation=" << operation_id
      << "\trows=" << row_count;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << '\n';
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool TableHasDeferredKeyConstraint(const CrudTableRecord& table) {
  for (const auto& [column_name, descriptor] : table.columns) {
    (void)column_name;
    const std::string lower = LowerAscii(descriptor);
    const bool key_like = lower.find("primary_key") != std::string::npos ||
                          lower.find("unique_key") != std::string::npos ||
                          lower.find("unique=true") != std::string::npos ||
                          lower.find("pk=true") != std::string::npos;
    const bool deferred = lower.find("deferrable=true") != std::string::npos ||
                          lower.find("initially_deferred") != std::string::npos ||
                          lower.find("enforcement_timing=deferred") != std::string::npos ||
                          lower.find("enforcement_timing=transaction_end") != std::string::npos;
    if (key_like && deferred) { return true; }
  }
  return false;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IsUniqueIndexForConflict(const CrudIndexRecord& index) {
  return index.unique ||
         std::find(index.key_envelopes.begin(), index.key_envelopes.end(), "unique") !=
             index.key_envelopes.end();
}

struct BulkValidationEvidenceCompactor {
  bool enabled = false;
  EngineApiU64 input_row_count = 0;
  EngineApiU64 total_compacted_entries = 0;
  std::map<std::string, EngineApiU64> counts_by_kind;
  std::vector<EngineEvidenceReference> scratch;

  std::vector<EngineEvidenceReference>* CaptureTarget(
      std::vector<EngineEvidenceReference>* direct_target) {
    if (!enabled) {
      return direct_target;
    }
    scratch.clear();
    return &scratch;
  }

  void Record(const EngineEvidenceReference& evidence) {
    ++total_compacted_entries;
    ++counts_by_kind[evidence.evidence_kind];
  }

  void Record(const std::vector<EngineEvidenceReference>& evidence) {
    for (const auto& entry : evidence) {
      Record(entry);
    }
  }

  void FlushCapture() {
    if (!enabled) {
      return;
    }
    Record(scratch);
    scratch.clear();
  }

  void AppendOrCompact(const std::vector<EngineEvidenceReference>& evidence,
                       std::vector<EngineEvidenceReference>* direct_target) {
    if (!enabled) {
      if (direct_target != nullptr) {
        direct_target->insert(direct_target->end(), evidence.begin(), evidence.end());
      }
      return;
    }
    Record(evidence);
  }

  void PushOrCompact(EngineEvidenceReference evidence,
                     std::vector<EngineEvidenceReference>* direct_target) {
    if (!enabled) {
      if (direct_target != nullptr) {
        direct_target->push_back(std::move(evidence));
      }
      return;
    }
    Record(evidence);
  }

  void AddSummaryEvidence(std::vector<EngineEvidenceReference>* direct_target) const {
    if (!enabled || direct_target == nullptr) {
      return;
    }
    direct_target->push_back({"bulk_validation_evidence_compacted", "true"});
    direct_target->push_back({"bulk_validation_evidence_input_rows",
                              std::to_string(input_row_count)});
    direct_target->push_back({"bulk_validation_evidence_entry_count",
                              std::to_string(total_compacted_entries)});
    for (const auto& [kind, count] : counts_by_kind) {
      direct_target->push_back({"bulk_validation_evidence_count." + kind,
                                std::to_string(count)});
    }
  }
};

EngineApiDiagnostic UniqueConflictDiagnostic(const CrudTableRecord& table,
                                             const CrudIndexRecord& index) {
  bool primary_key = std::find(index.key_envelopes.begin(),
                              index.key_envelopes.end(),
                              "primary_key") != index.key_envelopes.end();
  bool unique_key = index.unique ||
                    std::find(index.key_envelopes.begin(),
                              index.key_envelopes.end(),
                              "unique") != index.key_envelopes.end();
  for (const auto& [column_name, descriptor] : table.columns) {
    if (column_name != index.column_name) {
      continue;
    }
    const std::string lowered = LowerAscii(descriptor);
    primary_key = primary_key ||
                  lowered.find("primary_key") != std::string::npos ||
                  lowered.find("pk=true") != std::string::npos;
    unique_key = unique_key ||
                 lowered.find("unique_key") != std::string::npos ||
                 lowered.find("unique=true") != std::string::npos;
    break;
  }
  if (primary_key) {
    return MakeEngineApiDiagnostic("CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION",
                                   "constraint.primary_key.violation",
                                   "duplicate_key:" + index.index_uuid);
  }
  if (unique_key) {
    return MakeEngineApiDiagnostic("CLI.CONSTRAINT_UNIQUE_VIOLATION",
                                   "constraint.unique.violation",
                                   "duplicate_key:" + index.index_uuid);
  }
  return MakeInvalidRequestDiagnostic("crud.unique_index", "unique_index_duplicate");
}

EngineApiU64 UniqueIndexCount(const std::vector<CrudIndexRecord>& indexes) {
  EngineApiU64 count = 0;
  for (const auto& index : indexes) {
    if (IsUniqueIndexForConflict(index)) {
      ++count;
    }
  }
  return count;
}

void AppendRowLocatorStreamEvidence(
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

DmlTargetAccessPlanRequest BuildOnConflictLocatorPlanRequest(
    const EngineInsertRowsRequest& request,
    const std::string& table_uuid,
    const std::string& row_uuid) {
  DmlTargetAccessPlanRequest plan_request;
  plan_request.mutation_kind = "dml.insert_rows.on_conflict";
  plan_request.database_uuid = request.context.database_uuid.canonical;
  plan_request.relation_uuid = table_uuid;
  plan_request.relation_present = true;
  plan_request.predicate_kind = "row_uuid_match";
  plan_request.predicate_descriptor_digest = "on_conflict_row_uuid:" + row_uuid;
  plan_request.row_uuid = row_uuid;
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
  plan_request.mga_visibility_recheck_planned = true;
  plan_request.security_recheck_planned = true;
  plan_request.grants_proven = request.context.security_context_present;
  plan_request.security_context_present = request.context.security_context_present;
  plan_request.parser_or_reference_authority = false;
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
  plan_request.estimated_rows = 1;
  return plan_request;
}

DmlRowLocatorStreamResult BuildOnConflictRowLocatorStream(
    const EngineInsertRowsRequest& request,
    const std::string& table_uuid,
    const std::string& row_uuid) {
  const auto plan_request =
      BuildOnConflictLocatorPlanRequest(request, table_uuid, row_uuid);
  auto plan = BuildDmlTargetAccessPlan(plan_request);
  DmlRowLocatorStreamRequest stream_request;
  stream_request.consumer = DmlRowLocatorStreamConsumer::update;
  stream_request.access_plan = std::move(plan);
  stream_request.access_plan_engine_authority_proof = true;
  stream_request.durable_mga_inventory_proof = true;
  stream_request.mga_visibility_recheck_planned = true;
  stream_request.security_recheck_planned = true;
  stream_request.parser_or_reference_authority = false;
  stream_request.index_or_cache_finality_authority = false;
  return BuildDmlRowLocatorStream(stream_request);
}

std::vector<std::string> ConflictIndexColumns(const CrudIndexRecord& index) {
  std::vector<std::string> columns;
  for (const auto& envelope : index.key_envelopes) {
    if (envelope.empty() || envelope == "unique" || envelope == "primary_key" ||
        StartsWith(envelope, "include:") || StartsWith(envelope, "where_eq:") ||
        StartsWith(envelope, "where_mod_eq:") || envelope == "where_true") {
      continue;
    }
    if (StartsWith(envelope, "identity:")) {
      columns.push_back(envelope.substr(9));
    } else if (StartsWith(envelope, "desc:")) {
      columns.push_back(envelope.substr(5));
    } else if (StartsWith(envelope, "cast:")) {
      const std::string rest = envelope.substr(5);
      const auto pos = rest.find(':');
      columns.push_back(pos == std::string::npos ? rest : rest.substr(0, pos));
    } else {
      columns.push_back(envelope);
    }
  }
  if (columns.empty() && !index.column_name.empty()) { columns.push_back(index.column_name); }
  return columns;
}

std::string InsertOptionValue(const EngineInsertRowsRequest& request, std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) { return option.substr(prefix.size()); }
  }
  return {};
}

std::vector<std::string> InsertOptionValues(const EngineInsertRowsRequest& request,
                                            std::string_view prefix) {
  std::vector<std::string> values;
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) { values.push_back(option.substr(prefix.size())); }
  }
  return values;
}

std::string ConflictAction(const EngineInsertRowsRequest& request) {
  if (!request.on_conflict_action.empty()) return LowerAscii(request.on_conflict_action);
  const std::string option_action = InsertOptionValue(request, "on_conflict_action:");
  if (!option_action.empty()) return LowerAscii(option_action);
  const std::string duplicate_mode = LowerAscii(request.duplicate_mode);
  if (duplicate_mode == "ignore") return "do_nothing";
  if (duplicate_mode == "update") return "do_update";
  return {};
}

std::string ConflictTargetColumn(const EngineInsertRowsRequest& request,
                                 const std::vector<CrudIndexRecord>& visible_indexes) {
  if (!request.conflict_target_column.empty()) return request.conflict_target_column;
  const std::string option_target = InsertOptionValue(request, "conflict_target_column:");
  if (!option_target.empty()) return option_target;
  for (const auto& index : visible_indexes) {
    if (!IsUniqueIndexForConflict(index)) { continue; }
    const auto columns = ConflictIndexColumns(index);
    if (columns.size() == 1) return columns.front();
  }
  return {};
}

std::optional<CrudIndexRecord> FindConflictTargetIndex(
    const std::vector<CrudIndexRecord>& visible_indexes,
    const std::string& target_column) {
  for (const auto& index : visible_indexes) {
    if (!IsUniqueIndexForConflict(index)) { continue; }
    const auto columns = ConflictIndexColumns(index);
    if (columns.size() == 1 && columns.front() == target_column) { return index; }
  }
  return std::nullopt;
}

std::vector<std::string> ConflictUpdateColumns(
    const EngineInsertRowsRequest& request,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& target_column) {
  if (!request.conflict_update_columns.empty()) return request.conflict_update_columns;
  auto option_columns = InsertOptionValues(request, "conflict_update_column:");
  if (!option_columns.empty()) return option_columns;
  option_columns = InsertOptionValues(request, "on_conflict_update_column:");
  if (!option_columns.empty()) return option_columns;
  if (!InsertOptionValue(request, "on_conflict_assignment_plan:").empty()) {
    return {};
  }
  for (const auto& [field, ignored] : values) {
    (void)ignored;
    if (field != target_column) { option_columns.push_back(field); }
  }
  return option_columns;
}

std::vector<std::string> SplitText(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::optional<EngineApiU64> ParseInsertOptionU64(const EngineInsertRowsRequest& request,
                                                 std::string_view prefix) {
  const std::string value = InsertOptionValue(request, prefix);
  if (value.empty()) return std::nullopt;
  try {
    return static_cast<EngineApiU64>(std::stoull(value));
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<std::string> GeneratedInsertSelectSourceUuids(
    const EngineInsertRowsRequest& request) {
  std::vector<std::string> source_uuids;
  for (const auto prefix : {"insert_select_source_uuid_0:",
                            "insert_select_source_uuid_1:"}) {
    const std::string uuid = InsertOptionValue(request, prefix);
    if (!uuid.empty()) { source_uuids.push_back(uuid); }
  }
  return source_uuids;
}

std::optional<EngineApiU64> GeneratedCounterRowCount(EngineApiU64 start,
                                                     EngineApiU64 step,
                                                     EngineApiU64 limit) {
  if (step == 0 || limit < start) return std::nullopt;
  return ((limit - start) / step) + 1;
}

EngineTypedValue GeneratedInsertValue(std::string value, std::string type_name) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = std::move(type_name);
  if (typed.descriptor.canonical_type_name.empty()) {
    typed.descriptor.canonical_type_name = "text";
  }
  typed.descriptor.encoded_descriptor = "type=" + typed.descriptor.canonical_type_name;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  typed.state = EngineValueState::value;
  return typed;
}

std::string GeneratedProjectionValue(const std::string& descriptor, EngineApiU64 counter) {
  if (descriptor == "counter") return std::to_string(counter);
  const auto parts = SplitText(descriptor, ':');
  if (parts.empty()) return {};
  if (parts[0] == "literal_text" && parts.size() >= 2) {
    return parts[1];
  }
  if (parts[0] == "literal_boolean" && parts.size() == 2) {
    return parts[1];
  }
  if (parts[0] == "literal_integer" && parts.size() == 2) {
    return parts[1];
  }
  if (parts[0] == "mod" && parts.size() == 2) {
    try {
      const auto modulus = static_cast<EngineApiU64>(std::stoull(parts[1]));
      return modulus == 0 ? std::string{} : std::to_string(counter % modulus);
    } catch (...) {
      return {};
    }
  }
  if (parts[0] == "prefix_counter" && parts.size() >= 2) {
    return parts[1] + std::to_string(counter);
  }
  if (parts[0] == "prefix_counter_offset" && parts.size() == 3) {
    try {
      const auto adjusted = static_cast<long long>(counter) + std::stoll(parts[2]);
      if (adjusted < 0) return {};
      return parts[1] + std::to_string(adjusted);
    } catch (...) {
      return {};
    }
  }
  if (parts[0] == "case_zero_literal_else_literal" && parts.size() == 3) {
    return counter == 0 ? parts[1] : parts[2];
  }
  if (parts[0] == "case_zero_literal_else_prefix_counter_offset" &&
      parts.size() == 4) {
    if (counter == 0) return parts[1];
    try {
      const auto adjusted = static_cast<long long>(counter) + std::stoll(parts[3]);
      if (adjusted < 0) return {};
      return parts[2] + std::to_string(adjusted);
    } catch (...) {
      return {};
    }
  }
  if (parts[0] == "cast_divide" && parts.size() >= 4) {
    try {
      const long double divisor = std::stold(parts[2]);
      const int scale = std::stoi(parts[3]);
      if (divisor == 0.0L || scale < 0 || scale > 18) return {};
      std::ostringstream out;
      out << std::fixed << std::setprecision(scale)
          << (static_cast<long double>(counter) / divisor);
      return out.str();
    } catch (...) {
      return {};
    }
  }
  if (parts[0] == "counter_multiply" && parts.size() >= 3) {
    try {
      const long double factor = std::stold(parts[1]);
      const int scale = std::stoi(parts[2]);
      if (scale < 0 || scale > 18) return {};
      std::ostringstream out;
      out << std::fixed << std::setprecision(scale)
          << (static_cast<long double>(counter) * factor);
      return out.str();
    } catch (...) {
      return {};
    }
  }
  if (parts[0] == "mod_equals" && parts.size() == 3) {
    try {
      const auto modulus = static_cast<EngineApiU64>(std::stoull(parts[1]));
      const auto expected = static_cast<EngineApiU64>(std::stoull(parts[2]));
      if (modulus == 0) return {};
      return (counter % modulus) == expected ? "true" : "false";
    } catch (...) {
      return {};
    }
  }
  return {};
}

std::string GeneratedProjectionType(const std::string& descriptor,
                                    const std::string& target_descriptor) {
  if (descriptor == "counter") return "integer";
  if (descriptor.rfind("literal_text:", 0) == 0) return "text";
  if (descriptor.rfind("literal_boolean:", 0) == 0) return "boolean";
  if (descriptor.rfind("literal_integer:", 0) == 0) return "integer";
  if (descriptor.rfind("mod:", 0) == 0) return "integer";
  if (descriptor.rfind("prefix_counter:", 0) == 0) return "text";
  if (descriptor.rfind("prefix_counter_offset:", 0) == 0) return "text";
  if (descriptor.rfind("case_zero_literal_else_", 0) == 0) return "text";
  if (descriptor.rfind("cast_divide:", 0) == 0) return "decimal";
  if (descriptor.rfind("counter_multiply:", 0) == 0) return "decimal";
  if (descriptor.rfind("mod_equals:", 0) == 0) return "boolean";
  if (target_descriptor.rfind("type=", 0) == 0) return target_descriptor.substr(5);
  return target_descriptor.empty() ? "text" : target_descriptor;
}

std::vector<EngineRowValue> BuildRecursiveCounterInsertRows(
    const EngineInsertRowsRequest& request,
    const CrudTableRecord& table,
    const CrudState& state,
    EngineApiDiagnostic* diagnostic,
    std::vector<EngineEvidenceReference>* evidence) {
  if (diagnostic != nullptr) {
    *diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  const std::string source_kind =
      InsertOptionValue(request, "insert_select_source_kind:");
  if (source_kind != "recursive_counter_cte") return {};
  const auto start = ParseInsertOptionU64(request, "insert_select_counter_start:");
  const auto step = ParseInsertOptionU64(request, "insert_select_counter_step:");
  const auto limit = ParseInsertOptionU64(request, "insert_select_counter_limit:");
  const auto projection_count =
      ParseInsertOptionU64(request, "insert_select_projection_count:");
  if (!start || !step || *step == 0 || !limit || !projection_count ||
      *projection_count == 0 || *projection_count > table.columns.size()) {
    if (diagnostic != nullptr) {
      *diagnostic = MakeInvalidRequestDiagnostic("dml.insert_rows",
                                                 "insert_select_generator_descriptor_invalid");
    }
    return {};
  }
  if (*limit < *start || ((*limit - *start) / *step) + 1 > 1000000ULL) {
    if (diagnostic != nullptr) {
      *diagnostic = MakeInvalidRequestDiagnostic("dml.insert_rows",
                                                 "insert_select_generator_bound_refused");
    }
    return {};
  }
  const auto generated_count = GeneratedCounterRowCount(*start, *step, *limit);
  if (!generated_count) {
    if (diagnostic != nullptr) {
      *diagnostic = MakeInvalidRequestDiagnostic("dml.insert_rows",
                                                 "insert_select_generator_bound_refused");
    }
    return {};
  }

  const std::vector<std::string> source_uuids =
      GeneratedInsertSelectSourceUuids(request);
  if (!source_uuids.empty()) {
    EngineApiU64 visible_capacity = 1;
    for (const auto& source_uuid : source_uuids) {
      const auto source_table = FindVisibleCrudTable(state,
                                                     source_uuid,
                                                     request.context.local_transaction_id);
      if (!source_table) {
        if (diagnostic != nullptr) {
          *diagnostic =
              MakeInvalidRequestDiagnostic("dml.insert_rows",
                                           "insert_select_source_table_not_visible");
        }
        return {};
      }
      const auto source_rows =
          VisibleCrudRowsForContext(state, source_uuid, request.context);
      const auto source_count = static_cast<EngineApiU64>(source_rows.size());
      if (source_count == 0) {
        visible_capacity = 0;
        break;
      }
      const EngineApiU64 required = *generated_count;
      if (visible_capacity >= required ||
          source_count >= ((required + visible_capacity - 1) / visible_capacity)) {
        visible_capacity = required;
      } else {
        visible_capacity *= source_count;
      }
    }
    if (evidence != nullptr) {
      evidence->push_back({"insert_select_source_capacity_checked", "true"});
      evidence->push_back({"insert_select_source_required_rows",
                           std::to_string(*generated_count)});
      evidence->push_back({"insert_select_source_visible_capacity",
                           std::to_string(visible_capacity)});
    }
    if (visible_capacity < *generated_count) {
      if (diagnostic != nullptr) {
        *diagnostic =
            MakeInvalidRequestDiagnostic("dml.insert_rows",
                                         "insert_select_source_capacity_insufficient");
      }
      return {};
    }
  }

  std::vector<std::string> projections;
  projections.reserve(static_cast<std::size_t>(*projection_count));
  for (EngineApiU64 index = 0; index < *projection_count; ++index) {
    const std::string descriptor =
        InsertOptionValue(request, "insert_select_projection_" + std::to_string(index) + ":");
    if (descriptor.empty()) {
      if (diagnostic != nullptr) {
        *diagnostic = MakeInvalidRequestDiagnostic("dml.insert_rows",
                                                   "insert_select_projection_descriptor_missing");
      }
      return {};
    }
    projections.push_back(descriptor);
  }

  std::vector<EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(*generated_count));
  for (EngineApiU64 counter = *start; counter <= *limit; counter += *step) {
    EngineRowValue row;
    row.requested_row_uuid.canonical = GenerateCrudEngineUuid("row");
    for (std::size_t column = 0; column < projections.size(); ++column) {
      const auto& target_column = table.columns[column];
      const std::string value = GeneratedProjectionValue(projections[column], counter);
      if (value.empty() && projections[column] != "prefix_counter:") {
        if (diagnostic != nullptr) {
          *diagnostic = MakeInvalidRequestDiagnostic("dml.insert_rows",
                                                     "insert_select_projection_evaluation_failed");
        }
        return {};
      }
      row.fields.push_back({
          target_column.first,
          GeneratedInsertValue(value,
                               GeneratedProjectionType(projections[column],
                                                       target_column.second))});
    }
    rows.push_back(std::move(row));
    if (*limit - counter < *step) break;
  }
  return rows;
}

struct InsertConflictAssignmentExpression {
  std::string target_column;
  std::string source_column;
  std::string operation;
  std::string literal_value;
  std::string literal_type;
};

std::vector<InsertConflictAssignmentExpression> ParseInsertConflictAssignmentPlan(
    const EngineInsertRowsRequest& request,
    bool* invalid) {
  if (invalid != nullptr) { *invalid = false; }
  const std::string plan = InsertOptionValue(request, "on_conflict_assignment_plan:");
  if (plan.empty()) { return {}; }

  std::vector<InsertConflictAssignmentExpression> expressions;
  for (const auto& item : SplitText(plan, ';')) {
    const auto parts = SplitText(item, '|');
    if (parts.size() != 5 || parts[0].empty() || parts[2].empty()) {
      if (invalid != nullptr) { *invalid = true; }
      return {};
    }
    InsertConflictAssignmentExpression expression;
    expression.target_column = parts[0];
    expression.source_column = parts[1];
    expression.operation = parts[2];
    expression.literal_value = parts[3];
    expression.literal_type = parts[4];
    if (expression.operation != "literal" &&
        expression.operation != "add" &&
        expression.operation != "subtract") {
      if (invalid != nullptr) { *invalid = true; }
      return {};
    }
    if (expression.operation != "literal" && expression.source_column.empty()) {
      if (invalid != nullptr) { *invalid = true; }
      return {};
    }
    expressions.push_back(std::move(expression));
  }
  return expressions;
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

EngineApiDiagnostic ApplyInsertConflictAssignmentExpressions(
    const std::vector<InsertConflictAssignmentExpression>& expressions,
    std::vector<std::pair<std::string, std::string>>* values) {
  if (values == nullptr) {
    return MakeInvalidRequestDiagnostic("dml.insert_rows",
                                        "on_conflict_assignment_values_required");
  }
  for (const auto& expression : expressions) {
    std::string new_value = expression.literal_value;
    if (expression.operation == "add" || expression.operation == "subtract") {
      const std::string source_value = CrudFieldValue(*values, expression.source_column);
      long double left = 0.0;
      long double right = 0.0;
      if (!ParseLongDoubleValue(source_value, &left) ||
          !ParseLongDoubleValue(expression.literal_value, &right)) {
        return MakeInvalidRequestDiagnostic(
            "dml.insert_rows",
            "on_conflict_assignment_arithmetic_requires_numeric_values");
      }
      const long double computed =
          expression.operation == "subtract" ? left - right : left + right;
      new_value = FormatArithmeticResult(
          computed,
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

bool ReplaceValueFromExcluded(std::vector<std::pair<std::string, std::string>>* target_values,
                              const std::vector<std::pair<std::string, std::string>>& excluded_values,
                              const std::string& column) {
  const std::string excluded_value = CrudFieldValue(excluded_values, column);
  bool replaced = false;
  for (auto& [field, value] : *target_values) {
    if (field == column) {
      value = excluded_value;
      replaced = true;
    }
  }
  if (!replaced) { target_values->push_back({column, excluded_value}); }
  return true;
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

EngineInsertRowsResult AllocationFailureResult(const EngineRequestContext& context,
                                               const DmlPageAllocationRuntimeResult& allocation) {
  auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
      context,
      "dml.insert_rows",
      allocation.diagnostic);
  AddDmlPageAllocationRuntimeEvidence(allocation, &failure);
  return failure;
}

struct StagedInsertRow {
  CrudRowVersionRecord row_record;
  std::vector<std::pair<std::string, std::string>> logical_values;
  bool toast_required = false;
};

struct UniqueConflictProbeResult {
  CrudRowVersionRecord row;
  std::string index_uuid;
  std::string key_value;
  std::string candidate_source;
  std::string physical_probe_path;
};

struct UniquePreflightRouteValidation {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
};

struct InsertExecutionControlDecision {
  bool admitted = true;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
};

struct UniqueStatementOverlay {
  std::map<std::string, CrudRowVersionRecord> rows_by_uuid;
  std::map<std::string, std::map<std::string, std::set<std::string>>> key_rows_by_index_uuid;
};

struct UniquePhysicalProbeCache {
  std::map<std::string, std::map<std::string, std::set<std::string>>> key_rows_by_index_uuid;
  EngineApiU64 indexed_entry_count = 0;
  EngineApiU64 index_count = 0;
  mutable EngineApiU64 physical_probe_attempts = 0;
  mutable EngineApiU64 physical_probe_hits = 0;
  mutable EngineApiU64 physical_probe_misses = 0;
  mutable EngineApiU64 scan_fallback_attempts = 0;
  mutable EngineApiU64 scan_fallback_hits = 0;
};

UniquePhysicalProbeCache BuildUniquePhysicalProbeCache(
    const CrudState& state,
    const std::string& table_uuid,
    const EngineRequestContext& context,
    const std::vector<CrudIndexRecord>& indexes) {
  UniquePhysicalProbeCache cache;
  std::set<std::string> unique_index_uuids;
  for (const auto& index : indexes) {
    if (!IsUniqueIndexForConflict(index)) {
      continue;
    }
    unique_index_uuids.insert(index.index_uuid);
    cache.key_rows_by_index_uuid[index.index_uuid];
  }
  cache.index_count = static_cast<EngineApiU64>(unique_index_uuids.size());
  if (unique_index_uuids.empty()) {
    return cache;
  }
  for (const auto& entry : state.index_entries) {
    if (entry.table_uuid != table_uuid ||
        unique_index_uuids.count(entry.index_uuid) == 0 ||
        !CrudCreatorVisible(state,
                            entry.creator_tx,
                            entry.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    cache.key_rows_by_index_uuid[entry.index_uuid][entry.key_value].insert(entry.row_uuid);
    ++cache.indexed_entry_count;
  }
  return cache;
}

std::vector<CrudIndexRecord> SynchronousInsertIndexes(
    const InsertBatchContext& batch_context) {
  std::vector<CrudIndexRecord> indexes;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action == InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    indexes.push_back(entry.index);
  }
  return indexes;
}

bool IndexKeysChanged(const CrudIndexRecord& index,
                      const std::vector<std::pair<std::string, std::string>>& before,
                      const std::vector<std::pair<std::string, std::string>>& after) {
  return CrudIndexKeysForValues(index, before) != CrudIndexKeysForValues(index, after);
}

bool InsertOptionEnabled(const EngineInsertRowsRequest& request,
                         std::string_view option) {
  return std::find(request.option_envelopes.begin(),
                   request.option_envelopes.end(),
                   option) != request.option_envelopes.end();
}

bool InsertOptionTruthy(const EngineInsertRowsRequest& request,
                        std::string_view key) {
  const std::string key_text(key);
  if (InsertOptionEnabled(request, key_text + "=true") ||
      InsertOptionEnabled(request, key_text + ":true") ||
      InsertOptionEnabled(request, key_text + "=1") ||
      InsertOptionEnabled(request, key_text + ":1") ||
      InsertOptionEnabled(request, key_text + "=on") ||
      InsertOptionEnabled(request, key_text + ":on") ||
      InsertOptionEnabled(request, key_text + "=enabled") ||
      InsertOptionEnabled(request, key_text + ":enabled")) {
    return true;
  }
  const std::string colon_value = LowerAscii(InsertOptionValue(request, key_text + ":"));
  const std::string equals_value = LowerAscii(InsertOptionValue(request, key_text + "="));
  return colon_value == "true" || colon_value == "1" || colon_value == "on" ||
         colon_value == "enabled" || equals_value == "true" || equals_value == "1" ||
         equals_value == "on" || equals_value == "enabled";
}

std::string InsertExecutionControlReason(const EngineInsertRowsRequest& request,
                                         std::string_view default_reason) {
  std::string reason = InsertOptionValue(request, "execution.control_reason:");
  if (reason.empty()) reason = InsertOptionValue(request, "execution.control_reason=");
  if (reason.empty()) reason = InsertOptionValue(request, "dml.insert.control_reason:");
  if (reason.empty()) reason = InsertOptionValue(request, "dml.insert.control_reason=");
  if (reason.empty()) reason = std::string(default_reason);
  return reason;
}

InsertExecutionControlDecision EvaluateInsertExecutionControl(
    const EngineInsertRowsRequest& request,
    EngineApiU64 input_row_count) {
  InsertExecutionControlDecision decision;
  decision.evidence.push_back({"insert_execution_control_policy", "evaluated"});
  decision.evidence.push_back({"insert_execution_control_stage", "pre_write"});
  decision.evidence.push_back({"insert_execution_control_boundary",
                               "before_serializable_record_and_physical_write"});
  decision.evidence.push_back({"insert_execution_control_input_rows",
                               std::to_string(input_row_count)});

  const bool cancel_requested =
      InsertOptionTruthy(request, "execution.cancel_requested") ||
      InsertOptionTruthy(request, "dml.insert.cancel_requested") ||
      InsertOptionTruthy(request, "copy.cancel_requested");
  const bool timeout_elapsed =
      InsertOptionTruthy(request, "execution.timeout_elapsed") ||
      InsertOptionTruthy(request, "execution.deadline_exceeded") ||
      InsertOptionTruthy(request, "dml.insert.timeout_elapsed") ||
      InsertOptionTruthy(request, "dml.insert.deadline_exceeded") ||
      InsertOptionTruthy(request, "copy.timeout_elapsed") ||
      InsertOptionTruthy(request, "copy.deadline_exceeded");

  if (cancel_requested) {
    decision.admitted = false;
    const std::string reason =
        InsertExecutionControlReason(request, "cancel_requested_before_write");
    decision.diagnostic = MakeEngineApiDiagnostic("SB-IPAR-INSERT-CANCELLED",
                                                  "dml.insert.cancelled",
                                                  "dml.insert_rows:" + reason,
                                                  true);
    decision.evidence.push_back({"insert_execution_control_decision", "refuse"});
    decision.evidence.push_back({"insert_execution_control_reason", reason});
    decision.evidence.push_back({"insert_execution_control_retry_class", "cancelled"});
    decision.evidence.push_back({"insert_execution_control_rows_published", "0"});
    decision.evidence.push_back({"insert_execution_control_partial_publication", "false"});
    decision.evidence.push_back({"insert_execution_control_valid_transaction_boundary",
                                 "caller_transaction_unchanged"});
    return decision;
  }

  if (timeout_elapsed) {
    decision.admitted = false;
    const std::string reason =
        InsertExecutionControlReason(request, "timeout_before_write");
    decision.diagnostic = MakeEngineApiDiagnostic("SB-IPAR-INSERT-TIMEOUT",
                                                  "dml.insert.timeout",
                                                  "dml.insert_rows:" + reason,
                                                  true);
    decision.evidence.push_back({"insert_execution_control_decision", "refuse"});
    decision.evidence.push_back({"insert_execution_control_reason", reason});
    decision.evidence.push_back({"insert_execution_control_retry_class", "timeout"});
    decision.evidence.push_back({"insert_execution_control_rows_published", "0"});
    decision.evidence.push_back({"insert_execution_control_partial_publication", "false"});
    decision.evidence.push_back({"insert_execution_control_valid_transaction_boundary",
                                 "caller_transaction_unchanged"});
    return decision;
  }

  decision.evidence.push_back({"insert_execution_control_decision", "admit"});
  decision.evidence.push_back({"insert_execution_control_reason", "within_policy"});
  return decision;
}

bool InsertRequestedCompatibilityFullRelationStateLoad(
    const EngineInsertRowsRequest& request) {
  return InsertOptionEnabled(request, "relation_state_load=full") ||
         InsertOptionEnabled(request, "insert_relation_state=full");
}

bool InsertOpaqueColumnsAllowed(const EngineInsertRowsRequest& request) {
  if (InsertOptionEnabled(request, "bulk.allow_opaque_columns=true")) return true;
  const std::string structured = LowerAscii(
      InsertOptionValue(request, "bulk.allow_opaque_columns:"));
  return structured == "1" || structured == "true" || structured == "yes" ||
         structured == "on";
}

bool InsertRequiresFullRelationState(const EngineInsertRowsRequest& request,
                                     std::string_view conflict_action) {
  (void)request;
  (void)conflict_action;
  return false;
}

bool InsertOptionKeyPresent(const std::vector<std::string>& options,
                            std::string_view key) {
  const std::string equals_prefix = std::string(key) + "=";
  const std::string colon_prefix = std::string(key) + ":";
  for (const auto& option : options) {
    if (option == key ||
        option.rfind(equals_prefix, 0) == 0 ||
        option.rfind(colon_prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

bool InsertRowsShareFieldOrder(std::span<const EngineRowValue> input_rows) {
  if (input_rows.size() < 2) {
    return true;
  }
  const auto& first = input_rows.front().fields;
  for (std::size_t row_index = 1; row_index < input_rows.size(); ++row_index) {
    const auto& current = input_rows[row_index].fields;
    if (current.size() != first.size()) {
      return false;
    }
    for (std::size_t field_index = 0; field_index < first.size(); ++field_index) {
      if (current[field_index].first != first[field_index].first) {
        return false;
      }
    }
  }
  return true;
}

bool DirectPhysicalInsertRouteEligible(
    const EngineInsertRowsRequest& request,
    std::string_view conflict_action,
    std::span<const EngineRowValue> input_rows) {
  if (input_rows.size() < 2) {
    return false;
  }
  if (!conflict_action.empty()) {
    return false;
  }
  if (request.duplicate_mode != "error") {
    return false;
  }
  if (request.reference_unique_checks_relaxed ||
      request.reference_foreign_key_checks_relaxed ||
      InsertBatchOptionEnabled(request, "reference.unique_checks=0") ||
      InsertBatchOptionEnabled(request, "reference.foreign_key_checks=0")) {
    return false;
  }
  if (InsertOptionEnabled(request, "direct_physical_insert=disabled") ||
      InsertOptionEnabled(request, "insert.direct_physical=disabled")) {
    return false;
  }
  return true;
}

dml::DirectPhysicalBulkAppendRequest MakeDirectPhysicalInsertRequest(
    const EngineInsertRowsRequest& request,
    std::span<const EngineRowValue> input_rows) {
  dml::DirectPhysicalBulkAppendRequest direct;
  direct.context = request.context;
  direct.target_table = request.target_table;
  direct.borrowed_input_rows = input_rows;
  direct.option_envelopes = request.option_envelopes;
  if (InsertRowsShareFieldOrder(input_rows) &&
      !InsertOptionKeyPresent(direct.option_envelopes,
                              "sblr.canonical_rowset_shared_shape")) {
    direct.option_envelopes.push_back("sblr.canonical_rowset_shared_shape=true");
  }
  direct.diagnostic_options = request.diagnostic_options;
  direct.estimated_row_count = request.estimated_row_count == 0
                                   ? static_cast<EngineApiU64>(input_rows.size())
                                   : request.estimated_row_count;
  direct.lane_operation =
      request.insert_mode == "insert_select" ||
              InsertBatchOptionEnabled(request, "insert_mode=insert_select")
          ? "insert_select"
          : "insert_rows";
  direct.duplicate_mode = request.duplicate_mode;
  direct.require_generated_row_uuid = request.require_generated_row_uuid;
  direct.strict_bulk_load_requested = request.strict_bulk_load_requested;
  direct.direct_lane_enabled = true;
  return direct;
}

EngineInsertRowsResult ConvertDirectPhysicalInsertResult(
    const EngineInsertRowsRequest& request,
    dml::DirectPhysicalBulkAppendResult direct_result,
    std::vector<EngineEvidenceReference> prefix_evidence) {
  EngineInsertRowsResult result;
  result.ok = direct_result.ok;
  result.operation_id = "dml.insert_rows";
  result.diagnostics = std::move(direct_result.diagnostics);
  result.unsupported_features = std::move(direct_result.unsupported_features);
  result.result_shape = std::move(direct_result.result_shape);
  result.primary_object = direct_result.primary_object;
  result.catalog_row_uuid = direct_result.catalog_row_uuid;
  result.transaction_uuid = direct_result.transaction_uuid;
  result.local_transaction_id = direct_result.local_transaction_id;
  result.dml_summary = direct_result.dml_summary;
  result.embedded_trust_mode_observed =
      direct_result.embedded_trust_mode_observed;
  result.cluster_authority_required = direct_result.cluster_authority_required;
  result.inserted_count = direct_result.inserted_rows;
  result.row_uuids = std::move(direct_result.row_uuids);
  result.evidence = std::move(prefix_evidence);
  result.evidence.push_back({"insert_direct_physical_bulk_route", "selected"});
  result.evidence.push_back({"insert_direct_physical_bulk_guard",
                             "serializable_and_security_checked"});
  result.evidence.insert(result.evidence.end(),
                         direct_result.evidence.begin(),
                         direct_result.evidence.end());
  result.evidence.push_back({"dml_returning", "affected_rows"});
  result.dml_summary.rows_changed = result.inserted_count;
  (void)request;
  return result;
}

struct DirectPhysicalInsertAttempt {
  bool attempted = false;
  EngineInsertRowsResult result;
};

DirectPhysicalInsertAttempt TryDirectPhysicalInsertRoute(
    const EngineInsertRowsRequest& request,
    std::string_view conflict_action,
    std::span<const EngineRowValue> input_rows,
    std::vector<EngineEvidenceReference> prefix_evidence = {}) {
  DirectPhysicalInsertAttempt attempt;
  if (!DirectPhysicalInsertRouteEligible(request, conflict_action, input_rows)) {
    return attempt;
  }
  attempt.attempted = true;
  auto phase_last = InsertApiSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(6);
  const auto mark_phase = [&](std::string phase) {
    const auto now = InsertApiSteadyClock::now();
    phase_micros.push_back(
        {std::move(phase), InsertApiElapsedMicros(phase_last, now)});
    phase_last = now;
  };

  auto serializable_admission = dml::CheckSerializableInsertMutation(
      request.context,
      "dml.insert_rows",
      request.target_table.uuid.canonical,
      input_rows,
      request.option_envelopes);
  mark_phase("serializable_check");
  if (!serializable_admission.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        serializable_admission.diagnostic);
    failure.evidence = std::move(prefix_evidence);
    failure.evidence.insert(failure.evidence.end(),
                            serializable_admission.evidence.begin(),
                            serializable_admission.evidence.end());
    attempt.result = std::move(failure);
    return attempt;
  }

  const auto execution_control = EvaluateInsertExecutionControl(
      request,
      static_cast<EngineApiU64>(input_rows.size()));
  mark_phase("execution_control");
  if (!execution_control.admitted) {
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        execution_control.diagnostic);
    failure.evidence = std::move(prefix_evidence);
    failure.evidence.insert(failure.evidence.end(),
                            execution_control.evidence.begin(),
                            execution_control.evidence.end());
    attempt.result = std::move(failure);
    return attempt;
  }

  auto serializable_recorded = dml::RecordSerializableInsertMutation(
      request.context,
      "dml.insert_rows",
      request.target_table.uuid.canonical,
      input_rows,
      request.option_envelopes);
  mark_phase("serializable_record");
  if (!serializable_recorded.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        serializable_recorded.diagnostic);
    failure.evidence = std::move(prefix_evidence);
    failure.evidence.insert(failure.evidence.end(),
                            serializable_admission.evidence.begin(),
                            serializable_admission.evidence.end());
    failure.evidence.insert(failure.evidence.end(),
                            execution_control.evidence.begin(),
                            execution_control.evidence.end());
    failure.evidence.insert(failure.evidence.end(),
                            serializable_recorded.evidence.begin(),
                            serializable_recorded.evidence.end());
    attempt.result = std::move(failure);
    return attempt;
  }

  prefix_evidence.insert(prefix_evidence.end(),
                         serializable_admission.evidence.begin(),
                         serializable_admission.evidence.end());
  prefix_evidence.insert(prefix_evidence.end(),
                         execution_control.evidence.begin(),
                         execution_control.evidence.end());
  prefix_evidence.insert(prefix_evidence.end(),
                         serializable_recorded.evidence.begin(),
                         serializable_recorded.evidence.end());
  auto direct_request = MakeDirectPhysicalInsertRequest(request, input_rows);
  mark_phase("make_direct_request");
  auto direct_result = dml::ExecuteDirectPhysicalBulkAppend(std::move(direct_request));
  mark_phase("execute_direct_physical_bulk_append");
  attempt.result = ConvertDirectPhysicalInsertResult(
      request,
      std::move(direct_result),
      std::move(prefix_evidence));
  mark_phase("convert_direct_result");
  WriteInsertApiPhaseTrace("try_direct_physical_insert_route",
                           "dml.insert_rows",
                           input_rows.size(),
                           phase_micros);
  return attempt;
}

bool RuntimeInsertPolicyApplies(const EngineMaterializedAuthorizationPolicy& policy,
                                const std::string& table_uuid) {
  if (!policy.requires_runtime_recheck) {
    return false;
  }
  if (!policy.right.empty() && policy.right != "INSERT") {
    return false;
  }
  return policy.target_uuid.canonical.empty() ||
         policy.target_uuid.canonical == table_uuid;
}

struct InsertRuntimeSecurityPolicyDecision {
  bool ok = true;
  bool denied = false;
  bool filtered = false;
  std::string reason = "allow";
};

InsertRuntimeSecurityPolicyDecision EvaluateInsertRuntimeSecurityPolicyEnvelope(
    const std::string& envelope,
    const std::vector<std::pair<std::string, std::string>>& values) {
  InsertRuntimeSecurityPolicyDecision decision;
  std::string normalized = LowerAscii(envelope);
  if (StartsWith(normalized, "sblr_predicate:")) {
    normalized = normalized.substr(15);
  }
  if (normalized.empty() || normalized == "allow" || normalized == "true") {
    return decision;
  }
  if (normalized == "filter" || normalized == "tenant_visible") {
    decision.filtered = true;
    decision.reason = normalized;
    return decision;
  }
  if (normalized == "deny" || normalized == "false") {
    decision.denied = true;
    decision.reason = normalized;
    return decision;
  }

  const auto parts = SplitText(normalized, ':');
  if (parts.size() == 3 && parts[0] == "column_equals") {
    decision.filtered = true;
    decision.denied = CrudFieldValue(values, parts[1]) != parts[2];
    decision.reason = "column_equals:" + parts[1];
    return decision;
  }
  if (parts.size() == 3 && parts[0] == "column_not_equals") {
    decision.filtered = true;
    decision.denied = CrudFieldValue(values, parts[1]) == parts[2];
    decision.reason = "column_not_equals:" + parts[1];
    return decision;
  }

  decision.ok = false;
  decision.denied = true;
  decision.reason = "unsupported_runtime_policy_envelope";
  return decision;
}

EngineEvaluateDeepSecurityResult EvaluateInsertRuntimeSecurityRecheck(
    const EngineInsertRowsRequest& request,
    const std::string& table_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    std::vector<EngineEvidenceReference>* evidence) {
  std::string rls_policy = "allow";
  for (const auto& policy : request.context.authorization_context.policies) {
    if (!RuntimeInsertPolicyApplies(policy, table_uuid)) {
      continue;
    }
    const auto decision = EvaluateInsertRuntimeSecurityPolicyEnvelope(
        policy.canonical_policy_envelope,
        values);
    if (evidence != nullptr) {
      evidence->push_back({"insert_runtime_security_policy_evaluated",
                           policy.policy_uuid.canonical});
      evidence->push_back({"insert_runtime_security_policy_result",
                           decision.reason + ":" +
                               (decision.denied ? "deny" : "allow")});
    }
    if (!decision.ok) {
      EngineEvaluateDeepSecurityResult refused =
          MakeCrudDiagnosticResult<EngineEvaluateDeepSecurityResult>(
              request.context,
              "security.evaluate_deep_enforcement",
              MakeInvalidRequestDiagnostic("security.evaluate_deep_enforcement",
                                           decision.reason));
      refused.decision = "refused";
      refused.evidence = evidence == nullptr ? std::vector<EngineEvidenceReference>{}
                                             : *evidence;
      return refused;
    }
    if (decision.denied) {
      rls_policy = "deny";
      break;
    }
    if (decision.filtered) {
      rls_policy = "filter";
    }
  }

  EngineEvaluateDeepSecurityRequest security;
  security.context = request.context;
  security.target_object = request.target_object;
  if (security.target_object.uuid.canonical.empty()) {
    security.target_object.uuid.canonical = table_uuid;
  }
  security.phase = "executor";
  security.required_right = "INSERT";
  security.mutation = false;
  security.option_envelopes.push_back("phase:executor");
  security.option_envelopes.push_back("required_right:INSERT");
  security.option_envelopes.push_back("mutation:false");
  security.option_envelopes.push_back("rls_policy:" + rls_policy);
  auto result = EngineEvaluateDeepSecurity(security);
  if (evidence != nullptr) {
    evidence->insert(evidence->end(), result.evidence.begin(), result.evidence.end());
    evidence->push_back({"insert_runtime_security_recheck",
                         result.decision + ":rls=" + rls_policy});
  }
  return result;
}

std::uint64_t InsertOptionU64(const EngineInsertRowsRequest& request,
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

using InsertSteadyClock = std::chrono::steady_clock;

EngineApiU64 ElapsedMicros(InsertSteadyClock::time_point start,
                           InsertSteadyClock::time_point finish) {
  return static_cast<EngineApiU64>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

std::string PreallocationOutcome(const DmlPageAllocationRuntimeResult& allocation) {
  if (!allocation.active) {
    return "runtime_inactive";
  }
  if (!allocation.preallocation_requested) {
    return "reservation_only";
  }
  if (allocation.preallocation_refused) {
    return "preallocation_refused";
  }
  if (allocation.preallocation_capped) {
    return allocation.preallocation_granted ? "preallocation_capped"
                                            : "preallocation_cap_refused";
  }
  if (allocation.preallocation_granted ||
      allocation.granted_preallocation_pages != 0) {
    return "preallocated";
  }
  return "reservation_only";
}

std::string PreallocationFallbackReason(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family) {
  if (!allocation.active) {
    return family + "_page_allocation_runtime_inactive";
  }
  if (!allocation.preallocation_requested) {
    return {};
  }
  if (allocation.preallocation_refused) {
    return family + "_page_preallocation_refused";
  }
  if (allocation.preallocation_capped && !allocation.preallocation_granted) {
    return family + "_page_preallocation_cap_refused";
  }
  if (!allocation.preallocation_granted &&
      allocation.granted_preallocation_pages == 0) {
    return family + "_page_preallocation_reservation_only";
  }
  return {};
}

EngineApiU64 AllocationEvidenceU64(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& kind) {
  for (const auto& item : allocation.evidence) {
    if (item.evidence_kind != kind) {
      continue;
    }
    std::istringstream in(item.evidence_id);
    EngineApiU64 value = 0;
    in >> value;
    return in.fail() ? 0 : value;
  }
  return 0;
}

EngineApiU64 FilespaceGrowthPages(
    const DmlPageAllocationRuntimeResult& allocation) {
  return AllocationEvidenceU64(allocation,
                               "filespace_runtime_capacity_window_materialized");
}

void AddDmlAllocationSummaryCounters(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family,
    EngineApiU64 row_count,
    EngineDmlSummaryCounters* counters) {
  if (counters == nullptr || !allocation.active) {
    return;
  }
  if (family == "row") {
    counters->row_extent_reservations += row_count;
    counters->version_extent_reservations += row_count;
    counters->page_extent_reservations += allocation.requested_pages;
  } else if (family == "index") {
    counters->index_extent_reservations += allocation.requested_pages;
  }
  counters->preallocation_requests += allocation.preallocation_requested ? 1 : 0;
  counters->preallocation_granted_pages += allocation.granted_preallocation_pages;
  counters->preallocation_capped += allocation.preallocation_capped ? 1 : 0;
  counters->preallocation_refused += allocation.preallocation_refused ? 1 : 0;
}

void AddDmlAllocationResourceEvidence(
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family,
    EngineApiU64 elapsed_microseconds,
    std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  evidence->push_back({family + "_page_allocation_runtime",
                       allocation.active ? "active" : "inactive"});
  evidence->push_back({family + "_page_reservation_requested_pages",
                       std::to_string(allocation.requested_pages)});
  evidence->push_back({family + "_page_preallocation_requested",
                       allocation.preallocation_requested ? "true" : "false"});
  evidence->push_back({family + "_page_preallocation_granted_pages",
                       std::to_string(allocation.granted_preallocation_pages)});
  evidence->push_back({family + "_page_preallocation_outcome",
                       PreallocationOutcome(allocation)});
  evidence->push_back({family + "_page_preallocation_claim",
                       allocation.granted_preallocation_pages != 0
                           ? "physical_preallocated_pages"
                           : "reservation_or_no_runtime_only"});
  const std::string fallback = PreallocationFallbackReason(allocation, family);
  if (!fallback.empty()) {
    evidence->push_back({family + "_page_preallocation_degraded_reason",
                         fallback});
  }
  const EngineApiU64 growth_pages = FilespaceGrowthPages(allocation);
  const EngineApiU64 growth_agent_pages =
      AllocationEvidenceU64(allocation, "filespace_agent_granted_pages");
  evidence->push_back({family + "_filespace_growth_pages",
                       std::to_string(growth_pages)});
  if (growth_agent_pages != 0 || growth_pages != 0) {
    evidence->push_back({family + "_filespace_growth_agent_granted_pages",
                         std::to_string(growth_agent_pages)});
  }
  evidence->push_back({family + "_filespace_growth_claim",
                       growth_pages != 0
                           ? "capacity_window_materialized"
                           : (allocation.active ? "not_materialized"
                                                : "runtime_inactive")});
  evidence->push_back({family + "_allocation_stall_microseconds",
                       std::to_string(elapsed_microseconds)});
}

void RecordDmlAllocationResourceMetrics(
    const InsertBatchContext& batch_context,
    const DmlPageAllocationRuntimeResult& allocation,
    const std::string& family,
    EngineApiU64 elapsed_microseconds) {
  const std::string outcome = PreallocationOutcome(allocation);
  if (allocation.active && allocation.granted_preallocation_pages != 0) {
    (void)scratchbird::core::metrics::RecordInsertPreallocatedPages(
        static_cast<double>(allocation.granted_preallocation_pages),
        batch_context.target_object_uuid,
        InsertBatchModeName(batch_context.insert_mode),
        family,
        outcome,
        PreallocationFallbackReason(allocation, family).empty()
            ? "none"
            : PreallocationFallbackReason(allocation, family));
  }
  RecordInsertBatchMetric(batch_context,
                          "sb_dml_insert_allocation_stall_microseconds",
                          static_cast<double>(elapsed_microseconds),
                          allocation.active ? "ok" : "inactive",
                          family + "_page_allocation");
  const EngineApiU64 growth_pages = FilespaceGrowthPages(allocation);
  if (growth_pages != 0) {
    RecordInsertBatchMetric(batch_context,
                            "sb_filespace_insert_growth_request_total",
                            static_cast<double>(growth_pages),
                            "capacity_window_materialized",
                            family + "_filespace_growth");
    RecordInsertBatchMetric(batch_context,
                            "sb_filespace_insert_growth_wait_microseconds",
                            static_cast<double>(elapsed_microseconds),
                            "ok",
                            family + "_filespace_growth");
  }
  const std::string fallback = PreallocationFallbackReason(allocation, family);
  if (!fallback.empty() && allocation.preallocation_requested) {
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_slow_path_total",
                            1.0,
                            "resource_degraded",
                            fallback);
  }
}

struct InsertPreworkQueueItem {
  std::vector<std::pair<std::string, std::string>> logical_values;
  EngineApiU64 encoded_bytes = 0;
};

struct InsertPreworkAllocationRecord {
  DmlPageAllocationRuntimeResult allocation;
  std::string family;
  EngineApiU64 row_count = 0;
  EngineApiU64 elapsed_microseconds = 0;
};

struct InsertPreworkQueueStats {
  bool enabled = false;
  bool helper_thread_started = false;
  bool failed = false;
  EngineApiDiagnostic diagnostic;
  EngineApiU64 rows_enqueued = 0;
  EngineApiU64 bytes_enqueued = 0;
  EngineApiU64 row_prework_rows = 0;
  EngineApiU64 index_prework_rows = 0;
  EngineApiU64 max_depth = 0;
  EngineApiU64 wait_count = 0;
  std::vector<InsertPreworkAllocationRecord> allocations;
};

class InsertPreworkQueue {
 public:
  InsertPreworkQueue(const EngineInsertRowsRequest& request,
                     const InsertBatchContext& batch_context,
                     const CrudState& state,
                     EngineApiU64 input_row_count,
                     bool ordinary_insert)
      : request_(request),
        state_(state),
        max_rows_(ResolveMaxRows(request, batch_context)),
        max_bytes_(ResolveMaxBytes(request, batch_context)) {
    stats_.enabled = ShouldEnable(request, input_row_count, ordinary_insert);
    if (!stats_.enabled) {
      return;
    }
    stats_.helper_thread_started = true;
    worker_ = std::thread([this]() { WorkerLoop(); });
  }

  InsertPreworkQueue(const InsertPreworkQueue&) = delete;
  InsertPreworkQueue& operator=(const InsertPreworkQueue&) = delete;

  ~InsertPreworkQueue() { (void)Finish(); }

  bool Enqueue(std::vector<std::pair<std::string, std::string>> logical_values,
               EngineApiU64 encoded_bytes) {
    if (!stats_.enabled) {
      return true;
    }
    InsertPreworkQueueItem item{std::move(logical_values), encoded_bytes};
    std::unique_lock<std::mutex> lock(mutex_);
    auto fits = [&]() {
      const bool row_space = queue_.size() < max_rows_;
      const bool byte_space =
          queued_bytes_ + item.encoded_bytes <= max_bytes_ || queue_.empty();
      return row_space && byte_space;
    };
    while (!stop_requested_ && !stats_.failed && !fits()) {
      ++stats_.wait_count;
      space_available_.wait(lock);
    }
    if (stop_requested_ || stats_.failed) {
      return false;
    }
    queued_bytes_ += item.encoded_bytes;
    queue_.push_back(std::move(item));
    ++stats_.rows_enqueued;
    stats_.bytes_enqueued += encoded_bytes;
    stats_.max_depth =
        std::max<EngineApiU64>(stats_.max_depth,
                               static_cast<EngineApiU64>(queue_.size()));
    work_available_.notify_one();
    return true;
  }

  InsertPreworkQueueStats Finish() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = true;
      work_available_.notify_all();
      space_available_.notify_all();
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

 private:
  static bool DisabledByOption(const EngineInsertRowsRequest& request) {
    const std::string value =
        LowerAscii(InsertBatchOptionValue(request, "dml.insert_prework_queue="));
    const std::string insert_value =
        LowerAscii(InsertBatchOptionValue(request, "insert.prework_queue="));
    return value == "disabled" || value == "false" || value == "0" ||
           insert_value == "disabled" || insert_value == "false" ||
           insert_value == "0";
  }

  static bool ForcedByOption(const EngineInsertRowsRequest& request) {
    const std::string value =
        LowerAscii(InsertBatchOptionValue(request, "dml.insert_prework_queue="));
    const std::string insert_value =
        LowerAscii(InsertBatchOptionValue(request, "insert.prework_queue="));
    return value == "enabled" || value == "true" || value == "1" ||
           insert_value == "enabled" || insert_value == "true" ||
           insert_value == "1";
  }

  static EngineApiU64 ResolveMinRows(const EngineInsertRowsRequest& request) {
    EngineApiU64 min_rows =
        InsertOptionU64(request, "dml.insert_prework_queue.min_rows=", 8);
    min_rows = InsertOptionU64(request,
                               "insert.prework_queue.min_rows=",
                               min_rows);
    return std::max<EngineApiU64>(1, min_rows);
  }

  static EngineApiU64 ResolveMaxRows(const EngineInsertRowsRequest& request,
                                     const InsertBatchContext& batch_context) {
    const EngineApiU64 admitted =
        std::max<EngineApiU64>(1, batch_context.adaptive_batch_plan.admitted_rows);
    const EngineApiU64 default_rows =
        admitted > (std::numeric_limits<EngineApiU64>::max() / 2)
            ? admitted
            : std::max<EngineApiU64>(1024, admitted * 2);
    EngineApiU64 max_rows =
        InsertOptionU64(request,
                        "dml.insert_prework_queue.max_rows=",
                        default_rows);
    max_rows = InsertOptionU64(request,
                               "insert.prework_queue.max_rows=",
                               max_rows);
    return std::max<EngineApiU64>(1, max_rows);
  }

  static EngineApiU64 ResolveMaxBytes(const EngineInsertRowsRequest& request,
                                      const InsertBatchContext& batch_context) {
    const EngineApiU64 default_bytes = std::max<EngineApiU64>(
        1024 * 1024,
        batch_context.memory_policy.bulk_load_budget_bytes == 0
            ? 1024 * 1024
            : batch_context.memory_policy.bulk_load_budget_bytes / 2);
    EngineApiU64 max_bytes =
        InsertOptionU64(request,
                        "dml.insert_prework_queue.max_bytes=",
                        default_bytes);
    max_bytes = InsertOptionU64(request,
                                "insert.prework_queue.max_bytes=",
                                max_bytes);
    return std::max<EngineApiU64>(1, max_bytes);
  }

  static bool ShouldEnable(const EngineInsertRowsRequest& request,
                           EngineApiU64 input_row_count,
                           bool ordinary_insert) {
    if (!ordinary_insert) {
      return false;
    }
    if (DisabledByOption(request)) {
      return false;
    }
    if (ForcedByOption(request)) {
      return true;
    }
    return input_row_count >= ResolveMinRows(request);
  }

  void WorkerLoop() {
    for (;;) {
      std::deque<InsertPreworkQueueItem> work;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_available_.wait(lock, [&]() {
          return stop_requested_ || !queue_.empty() || stats_.failed;
        });
        if ((queue_.empty() && stop_requested_) || stats_.failed) {
          return;
        }
        while (!queue_.empty()) {
          queued_bytes_ -= queue_.front().encoded_bytes;
          work.push_back(std::move(queue_.front()));
          queue_.pop_front();
        }
        space_available_.notify_all();
      }
      ProcessWork(std::move(work));
    }
  }

  void ProcessWork(std::deque<InsertPreworkQueueItem> work) {
    if (work.empty()) {
      return;
    }
    const EngineApiU64 row_count =
        static_cast<EngineApiU64>(work.size());
    const auto row_allocation_start = InsertSteadyClock::now();
    auto row_allocation = ReserveDmlPageAllocationRuntime(
        request_.context,
        request_.option_envelopes,
        request_.target_table.uuid.canonical,
        DmlPageAllocationRuntimeFamily::row_data,
        row_count,
        "insert.prework.row_data");
    const EngineApiU64 row_allocation_elapsed =
        ElapsedMicros(row_allocation_start, InsertSteadyClock::now());
    if (!row_allocation.ok()) {
      SetFailure(row_allocation.diagnostic);
      return;
    }

    std::vector<std::vector<std::pair<std::string, std::string>>> index_value_batch;
    index_value_batch.reserve(work.size());
    for (auto& item : work) {
      index_value_batch.push_back(std::move(item.logical_values));
    }
    const auto index_allocation_start = InsertSteadyClock::now();
    auto index_allocation = ReserveDmlIndexPageAllocationRuntimeForRows(
        request_.context,
        request_.option_envelopes,
        state_,
        request_.target_table.uuid.canonical,
        index_value_batch,
        "insert.prework.index");
    const EngineApiU64 index_allocation_elapsed =
        ElapsedMicros(index_allocation_start, InsertSteadyClock::now());
    if (!index_allocation.ok()) {
      SetFailure(index_allocation.diagnostic);
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.allocations.push_back(
        {std::move(row_allocation), "row", row_count, row_allocation_elapsed});
    if (stats_.allocations.back().allocation.active) {
      stats_.row_prework_rows += row_count;
    }
    stats_.allocations.push_back({std::move(index_allocation),
                                  "index",
                                  0,
                                  index_allocation_elapsed});
    if (stats_.allocations.back().allocation.active) {
      stats_.index_prework_rows += row_count;
    }
  }

  void SetFailure(const EngineApiDiagnostic& diagnostic) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.failed = true;
    stats_.diagnostic = diagnostic;
    stop_requested_ = true;
    work_available_.notify_all();
    space_available_.notify_all();
  }

  const EngineInsertRowsRequest& request_;
  const CrudState& state_;
  const EngineApiU64 max_rows_ = 1;
  const EngineApiU64 max_bytes_ = 1;
  std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable space_available_;
  std::deque<InsertPreworkQueueItem> queue_;
  EngineApiU64 queued_bytes_ = 0;
  bool stop_requested_ = false;
  std::thread worker_;
  InsertPreworkQueueStats stats_;
};

void AddInsertPreworkQueueEvidence(const InsertPreworkQueueStats& stats,
                                   std::vector<EngineEvidenceReference>* evidence) {
  if (evidence == nullptr) {
    return;
  }
  evidence->push_back({"insert_transaction_queue_scope",
                       stats.enabled
                           ? "statement_with_transaction_authority"
                           : "not_enabled"});
  evidence->push_back({"insert_transaction_queue_return_before_flush", "false"});
  evidence->push_back({"insert_prework_queue_enabled",
                       stats.enabled ? "true" : "false"});
  evidence->push_back({"insert_prework_rows_enqueued",
                       std::to_string(stats.rows_enqueued)});
  evidence->push_back({"insert_prework_bytes_enqueued",
                       std::to_string(stats.bytes_enqueued)});
  evidence->push_back({"insert_prework_rows_prepared",
                       std::to_string(stats.row_prework_rows)});
  evidence->push_back({"insert_prework_index_rows_prepared",
                       std::to_string(stats.index_prework_rows)});
  evidence->push_back({"insert_prework_helper_thread_started",
                       stats.helper_thread_started ? "true" : "false"});
  evidence->push_back({"insert_prework_queue_max_depth",
                       std::to_string(stats.max_depth)});
  evidence->push_back({"insert_prework_queue_wait_count",
                       std::to_string(stats.wait_count)});
  evidence->push_back({"insert_prework_queue_commit_flush_contract",
                       "commit_waits_for_no_pending_statement_fenced"});
  evidence->push_back({"insert_prework_target_parallelism",
                       stats.enabled ? "target_table_queue_worker" : "not_enabled"});
  evidence->push_back({"insert_prework_cross_statement_queue",
                       "not_enabled_read_your_writes_overlay_required"});
  evidence->push_back({"mga_finality_authority",
                       "engine_transaction_inventory"});
  evidence->push_back({"parser_finality", "false"});
}

void AddInsertPreworkAllocationResults(const InsertPreworkQueueStats& stats,
                                       const InsertBatchContext& batch_context,
                                       EngineInsertRowsResult* result) {
  if (result == nullptr) {
    return;
  }
  for (const auto& record : stats.allocations) {
    AddDmlPageAllocationRuntimeEvidence(record.allocation, result);
    AddDmlAllocationSummaryCounters(record.allocation,
                                    record.family,
                                    record.row_count,
                                    &result->dml_summary);
    AddDmlAllocationResourceEvidence(record.allocation,
                                     record.family,
                                     record.elapsed_microseconds,
                                     &result->evidence);
    if (record.allocation.active) {
      ++result->dml_summary.page_reservations;
    }
    RecordDmlAllocationResourceMetrics(batch_context,
                                       record.allocation,
                                       record.family,
                                       record.elapsed_microseconds);
  }
}

bool UniquePreflightRouteRequired(const std::vector<CrudIndexRecord>& indexes,
                                  std::string_view conflict_action) {
  if (!conflict_action.empty()) {
    return true;
  }
  for (const auto& index : indexes) {
    if (IsUniqueIndexForConflict(index)) {
      return true;
    }
  }
  return false;
}

void AddUniquePreflightBaseEvidence(std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({"insert_unique_preflight_path", "index_backed"});
  evidence->push_back({"insert_unique_delta_overlay", "statement"});
  evidence->push_back({"insert_unique_mga_recheck", "required"});
  evidence->push_back({"insert_unique_security_recheck", "required"});
  evidence->push_back({"insert_unique_authority", "engine_mga"});
}

UniquePreflightRouteValidation ValidateUniquePreflightRoute(
    const EngineInsertRowsRequest& request,
    bool route_required) {
  UniquePreflightRouteValidation validation;
  if (!route_required) {
    validation.diagnostic =
        MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
    return validation;
  }

  AddUniquePreflightBaseEvidence(&validation.evidence);
  std::vector<std::string> refusals;
  auto refuse = [&refusals, &validation](std::string reason) {
    validation.evidence.push_back({"insert_unique_preflight_refusal", reason});
    refusals.push_back(std::move(reason));
  };

  const bool force_missing_security_context =
      InsertOptionEnabled(request, "odf033.force_missing_security_context=true");
  if (!request.context.security_context_present || force_missing_security_context) {
    refuse("missing security context");
  }
  if (InsertOptionEnabled(request, "odf033.disable_mga_visibility_recheck=true")) {
    refuse("missing MGA recheck");
  }
  if (InsertOptionEnabled(request, "odf033.disable_security_recheck=true")) {
    refuse("missing security recheck");
  }
  if (InsertOptionEnabled(request, "odf033.parser_or_reference_authority=true")) {
    refuse("unsafe parser/reference authority");
  }

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
  if (observed_catalog_epoch != 0 && request.context.catalog_generation_id != 0 &&
      observed_catalog_epoch != request.context.catalog_generation_id) {
    refuse("stale catalog epoch");
  }
  if (observed_security_epoch != 0 && request.context.security_epoch != 0 &&
      observed_security_epoch != request.context.security_epoch) {
    refuse("stale security epoch");
  }
  if (observed_policy_epoch != 0 && request.context.resource_epoch != 0 &&
      observed_policy_epoch != request.context.resource_epoch) {
    refuse("stale policy epoch");
  }

  const std::uint64_t observed_stats_epoch =
      InsertOptionU64(request, "odf033.observed_stats_epoch=", 0);
  const std::uint64_t current_stats_epoch =
      InsertOptionU64(request, "odf033.current_stats_epoch=", 0);
  if (observed_stats_epoch != 0 && current_stats_epoch != 0 &&
      observed_stats_epoch != current_stats_epoch) {
    refuse("stale stats epoch");
  }

  if (!refusals.empty()) {
    validation.ok = false;
    validation.evidence.push_back({"insert_unique_preflight_route", "refused"});
    validation.evidence.push_back({"insert_unique_preflight_fail_closed", "true"});
    validation.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.insert_rows",
        "unique_preflight_route_refused:" + refusals.front());
    return validation;
  }

  validation.evidence.push_back({"insert_unique_preflight_route", "accepted"});
  validation.diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  return validation;
}

EngineInsertRowsResult InsertDiagnosticResultWithEvidence(
    const EngineRequestContext& context,
    EngineApiDiagnostic diagnostic,
    const std::vector<EngineEvidenceReference>& evidence) {
  auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
      context,
      "dml.insert_rows",
      std::move(diagnostic));
  failure.evidence.insert(failure.evidence.end(), evidence.begin(), evidence.end());
  return failure;
}

bool RowHasUniqueIndexKey(const CrudIndexRecord& index,
                          const std::vector<std::pair<std::string, std::string>>& values,
                          const std::string& key_value) {
  const auto keys = CrudIndexKeysForValues(index, values);
  return std::find(keys.begin(), keys.end(), key_value) != keys.end();
}

std::optional<CrudRowVersionRecord> FindVisibleCrudRowUuidCandidate(
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

void RemoveUniqueStatementOverlayKeysForRow(
    UniqueStatementOverlay* overlay,
    const std::vector<CrudIndexRecord>& indexes,
    const CrudRowVersionRecord& row) {
  for (const auto& index : indexes) {
    if (!IsUniqueIndexForConflict(index)) {
      continue;
    }
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      auto found_index = overlay->key_rows_by_index_uuid.find(index.index_uuid);
      if (found_index == overlay->key_rows_by_index_uuid.end()) {
        continue;
      }
      auto found_key = found_index->second.find(key);
      if (found_key == found_index->second.end()) {
        continue;
      }
      found_key->second.erase(row.row_uuid);
      if (found_key->second.empty()) {
        found_index->second.erase(found_key);
      }
    }
  }
}

void UpsertUniqueStatementOverlayRow(UniqueStatementOverlay* overlay,
                                     const std::vector<CrudIndexRecord>& indexes,
                                     CrudRowVersionRecord row) {
  const auto existing = overlay->rows_by_uuid.find(row.row_uuid);
  if (existing != overlay->rows_by_uuid.end()) {
    RemoveUniqueStatementOverlayKeysForRow(overlay, indexes, existing->second);
  }
  const std::string row_uuid = row.row_uuid;
  for (const auto& index : indexes) {
    if (!IsUniqueIndexForConflict(index)) {
      continue;
    }
    for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
      overlay->key_rows_by_index_uuid[index.index_uuid][key].insert(row_uuid);
    }
  }
  overlay->rows_by_uuid[row_uuid] = std::move(row);
}

std::optional<UniqueConflictProbeResult> FindUniqueStatementOverlayConflict(
    const UniqueStatementOverlay& overlay,
    const CrudIndexRecord& index,
    const std::vector<std::string>& keys,
    const std::string& exclude_row_uuid) {
  const auto found_index = overlay.key_rows_by_index_uuid.find(index.index_uuid);
  if (found_index == overlay.key_rows_by_index_uuid.end()) {
    return std::nullopt;
  }
  for (const auto& key : keys) {
    const auto found_key = found_index->second.find(key);
    if (found_key == found_index->second.end()) {
      continue;
    }
    for (const auto& row_uuid : found_key->second) {
      if (row_uuid == exclude_row_uuid) {
        continue;
      }
      const auto found_row = overlay.rows_by_uuid.find(row_uuid);
      if (found_row == overlay.rows_by_uuid.end() ||
          found_row->second.deleted ||
          !RowHasUniqueIndexKey(index, found_row->second.values, key)) {
        continue;
      }
      return UniqueConflictProbeResult{found_row->second,
                                       index.index_uuid,
                                       key,
                                       "statement_delta_overlay",
                                       "statement_delta_overlay"};
    }
  }
  return std::nullopt;
}

std::optional<UniqueConflictProbeResult> FindPersistedUniqueIndexConflict(
    const CrudState& state,
    const std::string& table_uuid,
    const EngineRequestContext& context,
    const UniquePhysicalProbeCache& physical_probe_cache,
    const CrudIndexRecord& index,
    const std::vector<std::string>& keys,
    const std::string& exclude_row_uuid) {
  const auto found_index =
      physical_probe_cache.key_rows_by_index_uuid.find(index.index_uuid);
  if (found_index != physical_probe_cache.key_rows_by_index_uuid.end()) {
    ++physical_probe_cache.physical_probe_attempts;
    for (const auto& key : keys) {
      const auto found_key = found_index->second.find(key);
      if (found_key == found_index->second.end()) {
        continue;
      }
      for (const auto& row_uuid : found_key->second) {
        if (row_uuid == exclude_row_uuid) {
          continue;
        }
        const auto row = FindVisibleCrudRowUuidCandidate(state, table_uuid, row_uuid, context);
        if (row && RowHasUniqueIndexKey(index, row->values, key)) {
          ++physical_probe_cache.physical_probe_hits;
          return UniqueConflictProbeResult{*row,
                                           index.index_uuid,
                                           key,
                                           "persisted_unique_index_physical_probe",
                                           "mga_persisted_index_entry_lookup"};
        }
      }
    }
    ++physical_probe_cache.physical_probe_misses;
    return std::nullopt;
  }
  ++physical_probe_cache.scan_fallback_attempts;
  for (const auto& key : keys) {
    std::set<std::string> candidate_row_uuids;
    for (const auto& entry : state.index_entries) {
      if (entry.table_uuid != table_uuid ||
          entry.index_uuid != index.index_uuid ||
          entry.key_value != key ||
          entry.row_uuid == exclude_row_uuid ||
          !CrudCreatorVisible(state,
                              entry.creator_tx,
                              entry.event_sequence,
                              context.local_transaction_id)) {
        continue;
      }
      candidate_row_uuids.insert(entry.row_uuid);
    }
    for (const auto& row_uuid : candidate_row_uuids) {
      const auto row = FindVisibleCrudRowUuidCandidate(state, table_uuid, row_uuid, context);
      if (row && RowHasUniqueIndexKey(index, row->values, key)) {
        ++physical_probe_cache.scan_fallback_hits;
        return UniqueConflictProbeResult{*row,
                                         index.index_uuid,
                                         key,
                                         "persisted_unique_index_scan_fallback",
                                         "mga_index_entry_scan_fallback"};
      }
    }
  }
  return std::nullopt;
}

std::optional<UniqueConflictProbeResult> FindUniqueConflictByIndex(
    const CrudState& state,
    const std::string& table_uuid,
    const EngineRequestContext& context,
    const UniqueStatementOverlay& overlay,
    const UniquePhysicalProbeCache& physical_probe_cache,
    const CrudIndexRecord& index,
    const std::string& exclude_row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values) {
  const auto keys = CrudIndexKeysForValues(index, values);
  if (keys.empty()) {
    return std::nullopt;
  }
  if (const auto overlay_conflict =
          FindUniqueStatementOverlayConflict(overlay, index, keys, exclude_row_uuid)) {
    return overlay_conflict;
  }
  return FindPersistedUniqueIndexConflict(state,
                                          table_uuid,
                                          context,
                                          physical_probe_cache,
                                          index,
                                          keys,
                                          exclude_row_uuid);
}

EngineApiDiagnostic ValidateIndexBackedUniquePreflightForRow(
    const CrudState& state,
    const CrudTableRecord& table,
    const EngineRequestContext& context,
    const UniqueStatementOverlay& overlay,
    const UniquePhysicalProbeCache& physical_probe_cache,
    const std::vector<CrudIndexRecord>& indexes,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    ConstraintDmlValidationCache* constraint_cache,
    std::vector<EngineEvidenceReference>* evidence) {
  for (const auto& index : indexes) {
    if (!IsUniqueIndexForConflict(index)) {
      continue;
    }
    const auto conflict = FindUniqueConflictByIndex(state,
                                                    table.table_uuid,
                                                    context,
                                                    overlay,
                                                    physical_probe_cache,
                                                    index,
                                                    row_uuid,
                                                    values);
    if (conflict) {
      if (evidence != nullptr) {
        evidence->push_back({"insert_unique_probe_index", conflict->index_uuid});
        evidence->push_back({"insert_unique_probe_key", conflict->key_value});
        evidence->push_back({"insert_unique_probe_candidate_source",
                             conflict->candidate_source});
        evidence->push_back({"physical_unique_index_probe_path",
                             conflict->physical_probe_path});
        evidence->push_back({"insert_unique_mga_recheck", "row_uuid_candidate"});
        evidence->push_back({"unique_index_physical_probes",
                             std::to_string(physical_probe_cache.physical_probe_attempts)});
        evidence->push_back({"unique_index_physical_probe_hits",
                             std::to_string(physical_probe_cache.physical_probe_hits)});
        evidence->push_back({"unique_index_scan_fallbacks",
                             std::to_string(physical_probe_cache.scan_fallback_attempts)});
      }
      return UniqueConflictDiagnostic(table, index);
    }
  }
  for (const auto& index : indexes) {
    if (IsUniqueIndexForConflict(index)) {
      RecordIndexBackedUniquePreflightProof(constraint_cache,
                                            context,
                                            index,
                                            row_uuid,
                                            values,
                                            evidence);
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool ApplyStatementOverlayUpdateToStagedInsert(
    std::vector<StagedInsertRow>* staged_insert_rows,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& update_values,
    bool toast_required) {
  for (auto& staged : *staged_insert_rows) {
    if (staged.row_record.row_uuid != row_uuid) {
      continue;
    }
    staged.logical_values = update_values;
    staged.row_record.values = update_values;
    staged.toast_required = toast_required;
    return true;
  }
  return false;
}

// DPC_DEFERRED_INDEX_WRITE_PATH
std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> InsertDeltaEntries(
    const InsertBatchContext& batch_context,
    const CrudRowVersionRecord& row_record,
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action != InsertIndexMaintenanceAction::committed_delta_ledger) {
      continue;
    }
    MgaSecondaryIndexDeltaLedgerEntryInput input;
    input.index = entry.index;
    input.table_uuid = batch_context.target_object_uuid;
    input.row_uuid = row_record.row_uuid;
    input.version_uuid = row_record.version_uuid;
    input.values = values;
    input.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::insert;
    input.source_evidence_reference =
        "engine.dml.insert.secondary_index_delta:" + batch_context.statement_uuid;
    entries.push_back(std::move(input));
  }
  return entries;
}

std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> ConflictUpdateDeltaEntries(
    const InsertBatchContext& batch_context,
    const CrudRowVersionRecord& old_row,
    const std::string& new_version_uuid,
    const std::vector<std::pair<std::string, std::string>>& new_values) {
  std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> entries;
  for (const auto& entry : batch_context.index_plan.entries) {
    if (entry.action != InsertIndexMaintenanceAction::committed_delta_ledger ||
        !IndexKeysChanged(entry.index, old_row.values, new_values)) {
      continue;
    }
    MgaSecondaryIndexDeltaLedgerEntryInput before;
    before.index = entry.index;
    before.table_uuid = batch_context.target_object_uuid;
    before.row_uuid = old_row.row_uuid;
    before.version_uuid = old_row.version_uuid;
    before.values = old_row.values;
    before.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::update_before;
    before.source_evidence_reference =
        "engine.dml.insert.conflict_update.secondary_index_delta_before:" +
        batch_context.statement_uuid;
    entries.push_back(std::move(before));

    MgaSecondaryIndexDeltaLedgerEntryInput after;
    after.index = entry.index;
    after.table_uuid = batch_context.target_object_uuid;
    after.row_uuid = old_row.row_uuid;
    after.version_uuid = new_version_uuid;
    after.values = new_values;
    after.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::update_after;
    after.source_evidence_reference =
        "engine.dml.insert.conflict_update.secondary_index_delta_after:" +
        batch_context.statement_uuid;
    entries.push_back(std::move(after));
  }
  return entries;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_INSERT_API_STUBS

EngineInsertRowsResult EngineInsertRows(const EngineInsertRowsRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", MakeInvalidRequestDiagnostic("dml.insert_rows", "local_transaction_id_required"));
  }
  if (request.target_table.uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", MakeInvalidRequestDiagnostic("dml.insert_rows", "target_table_uuid_required"));
  }
  if (request.HasAmbiguousInputRows()) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", MakeInvalidRequestDiagnostic("dml.insert_rows", "input_rows_or_borrowed_rows_exclusive"));
  }
  auto insert_phase_last = InsertApiSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> insert_phase_micros;
  insert_phase_micros.reserve(12);
  const auto mark_insert_phase = [&](std::string phase) {
    const auto now = InsertApiSteadyClock::now();
    insert_phase_micros.push_back(
        {std::move(phase), InsertApiElapsedMicros(insert_phase_last, now)});
    insert_phase_last = now;
  };
  const auto write_insert_outer_trace = [&](std::size_t row_count) {
    WriteInsertApiPhaseTrace("engine_insert_rows",
                             "dml.insert_rows",
                             row_count,
                             insert_phase_micros);
  };
  const auto write_result_policy =
      ResolveWriteResultPolicy(request, "dml.insert_rows");
  mark_insert_phase("resolve_write_result_policy");
  if (!write_result_policy.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        write_result_policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  const std::string conflict_action = ConflictAction(request);
  mark_insert_phase("resolve_conflict_action");
  if (InsertRequestedCompatibilityFullRelationStateLoad(request)) {
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        MakeInvalidRequestDiagnostic("dml.insert_rows",
                                     "relation_state_full_load_diagnostic_only"));
    failure.evidence.push_back({"relation_state_full_load_refused", "diagnostic_only"});
    failure.evidence.push_back({"relation_state_full_loads", "0"});
    failure.evidence.push_back({"relation_state_scoped_loads", "0"});
    failure.evidence.push_back({"relation_state_load_reason",
                                "caller_requested_full_load_on_mutation_path"});
    return failure;
  }
  const auto direct_initial_input_rows = request.EffectiveInputRows();
  if (!direct_initial_input_rows.empty()) {
    auto direct_attempt = TryDirectPhysicalInsertRoute(request,
                                                      conflict_action,
                                                      direct_initial_input_rows);
    mark_insert_phase("direct_initial_attempt");
    if (direct_attempt.attempted) {
      write_insert_outer_trace(direct_initial_input_rows.size());
      return std::move(direct_attempt.result);
    }
  }
  const bool full_relation_state_required =
      InsertRequiresFullRelationState(request, conflict_action);
  const auto generated_source_uuids = GeneratedInsertSelectSourceUuids(request);
  std::vector<std::string> generated_insert_scope_uuids{
      request.target_table.uuid.canonical};
  generated_insert_scope_uuids.insert(generated_insert_scope_uuids.end(),
                                      generated_source_uuids.begin(),
                                      generated_source_uuids.end());
  mark_insert_phase("plan_relation_state_scope");
  auto loaded = full_relation_state_required
                    ? LoadMgaRelationStoreState(request.context)
                    : (!generated_source_uuids.empty()
                           ? LoadMgaRelationStoreStateForMutationTargets(
                                 request.context,
                                 generated_insert_scope_uuids)
                           : LoadMgaRelationStoreStateForInsertTarget(
                                 request.context,
                                 request.target_table.uuid.canonical));
  mark_insert_phase("load_relation_state");
  (void)scratchbird::core::metrics::RecordInsertRelationStateLoad(
      request.target_table.uuid.canonical,
      ResolveInsertBatchMode(request) == InsertBatchMode::singleton
          ? "singleton"
          : InsertBatchModeName(ResolveInsertBatchMode(request)),
      loaded.full_state_load,
      loaded.scoped_state_load,
      full_relation_state_required
          ? (conflict_action == "do_update"
                 ? "on_conflict_do_update_requires_child_reference_state"
                 : "request_required_full_state")
          : (!generated_source_uuids.empty()
                 ? "insert_select_target_source_scoped"
                 : (conflict_action == "do_update"
                        ? "insert_target_child_reference_scoped"
                        : "insert_target_scoped")));
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", loaded.diagnostic); }
  CrudState state = BuildCrudCompatibilityStateFromMga(std::move(loaded.state));
  const auto table = FindVisibleCrudTable(state, request.target_table.uuid.canonical, request.context.local_transaction_id);
  mark_insert_phase("build_state_and_find_table");
  if (!table) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", MakeInvalidRequestDiagnostic("dml.insert_rows", "target_table_not_visible"));
  }
  if (table->temporary && request.context.session_uuid.canonical.empty()) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        MakeInvalidRequestDiagnostic("dml.insert_rows",
                                     "temporary_table_requires_session_uuid"));
  }

  EngineApiDiagnostic generated_rows_diagnostic =
      MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  std::vector<EngineEvidenceReference> generated_rows_evidence;
  std::vector<EngineRowValue> generated_insert_select_rows;
  if (request.EffectiveInputRows().empty()) {
    generated_insert_select_rows =
        BuildRecursiveCounterInsertRows(request,
                                        *table,
                                        state,
                                        &generated_rows_diagnostic,
                                        &generated_rows_evidence);
  }
  mark_insert_phase("build_generated_insert_rows");
  if (generated_rows_diagnostic.error) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        generated_rows_diagnostic);
  }
  const std::span<const EngineRowValue> input_rows =
      generated_insert_select_rows.empty()
          ? request.EffectiveInputRows()
          : std::span<const EngineRowValue>(generated_insert_select_rows.data(),
                                            generated_insert_select_rows.size());
  if (input_rows.empty()) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", MakeInvalidRequestDiagnostic("dml.insert_rows", "at_least_one_row_required"));
  }

  if (!generated_insert_select_rows.empty()) {
    std::vector<EngineEvidenceReference> direct_prefix_evidence;
    direct_prefix_evidence.push_back({"insert_select_generator",
                                      "recursive_counter_cte"});
    direct_prefix_evidence.push_back({"insert_select_generated_row_count",
                                      std::to_string(generated_insert_select_rows.size())});
    direct_prefix_evidence.insert(direct_prefix_evidence.end(),
                                  generated_rows_evidence.begin(),
                                  generated_rows_evidence.end());
    auto direct_attempt = TryDirectPhysicalInsertRoute(request,
                                                      conflict_action,
                                                      input_rows,
                                                      std::move(direct_prefix_evidence));
    mark_insert_phase("direct_generated_attempt");
    if (direct_attempt.attempted) {
      write_insert_outer_trace(input_rows.size());
      return std::move(direct_attempt.result);
    }
  }

  if (CrudRowsTouchOpaqueColumn(*table, input_rows) &&
      !InsertOpaqueColumnsAllowed(request)) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        UnsupportedCrudFeatureDiagnostic("dml.insert_rows", "opaque_column_mutation_denied"));
  }
  auto serializable_admission = dml::CheckSerializableInsertMutation(
      request.context,
      "dml.insert_rows",
      request.target_table.uuid.canonical,
      input_rows,
      request.option_envelopes);
  if (!serializable_admission.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        serializable_admission.diagnostic);
    failure.evidence.insert(failure.evidence.end(),
                            serializable_admission.evidence.begin(),
                            serializable_admission.evidence.end());
    return failure;
  }
  const auto visible_indexes = VisibleCrudIndexesForTable(state, request.target_table.uuid.canonical, request.context.local_transaction_id);
  ConstraintDmlValidationCache constraint_cache;
  const bool unique_route_required =
      UniquePreflightRouteRequired(visible_indexes, conflict_action);
  const auto route_validation =
      ValidateUniquePreflightRoute(request, unique_route_required);
  if (!route_validation.ok) {
    return InsertDiagnosticResultWithEvidence(request.context,
                                              route_validation.diagnostic,
                                              route_validation.evidence);
  }
  MgaRelationStorageDescriptor relation_descriptor;
  const auto descriptor_ready = EnsureMgaRelationStorageDescriptor(request.context, *table, visible_indexes, &relation_descriptor);
  if (descriptor_ready.error) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", descriptor_ready);
  }
  InsertBatchContext batch_context = BeginInsertBatchContext(request, state, *table, visible_indexes);
  if (!batch_context.accepted) {
    const std::string fallback_reason =
        batch_context.fallback_reason.empty() ? "insert_batch_refused" : batch_context.fallback_reason;
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_batch_fallback_total",
                            1.0,
                            "fallback",
                            fallback_reason);
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        MakeInvalidRequestDiagnostic("dml.insert_rows", fallback_reason));
    AddInsertBatchEvidenceToResult(batch_context, &failure);
    failure.diagnostics.insert(failure.diagnostics.end(),
                               batch_context.diagnostics.begin(),
                               batch_context.diagnostics.end());
    AddDmlSummaryFallbackReason(&failure.dml_summary, fallback_reason);
    AddDmlSummaryEvidence(&failure);
    return failure;
  }
  const auto bulk_validation = ValidateStrictBulkLoadEligibility(batch_context, *table);
  if (bulk_validation.error) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", bulk_validation);
  }
  if (request.reference_unique_checks_relaxed || request.reference_foreign_key_checks_relaxed ||
      InsertBatchOptionEnabled(request, "reference.unique_checks=0") ||
      InsertBatchOptionEnabled(request, "reference.foreign_key_checks=0")) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        MakeInvalidRequestDiagnostic("dml.insert_rows", "reference_relaxer_requires_engine_policy"));
  }
  std::string conflict_target_column;
  std::optional<CrudIndexRecord> conflict_index;
  if (!conflict_action.empty()) {
    if (conflict_action != "do_nothing" && conflict_action != "do_update") {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
          request.context,
          "dml.insert_rows",
          MakeInvalidRequestDiagnostic("dml.insert_rows", "on_conflict_action_unsupported"));
    }
    conflict_target_column = ConflictTargetColumn(request, visible_indexes);
    if (conflict_target_column.empty()) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
          request.context,
          "dml.insert_rows",
          MakeInvalidRequestDiagnostic("dml.insert_rows", "on_conflict_target_required"));
    }
    conflict_index = FindConflictTargetIndex(visible_indexes, conflict_target_column);
    if (!conflict_index) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
          request.context,
          "dml.insert_rows",
          MakeInvalidRequestDiagnostic("dml.insert_rows", "on_conflict_target_unique_index_required"));
    }
  }
  const auto execution_control =
      EvaluateInsertExecutionControl(
          request,
          static_cast<EngineApiU64>(input_rows.size()));
  if (!execution_control.admitted) {
    return InsertDiagnosticResultWithEvidence(request.context,
                                              execution_control.diagnostic,
                                              execution_control.evidence);
  }
  auto result = MakeCrudSuccessResult<EngineInsertRowsResult>(request.context, "dml.insert_rows");
  result.evidence.insert(result.evidence.end(),
                         execution_control.evidence.begin(),
                         execution_control.evidence.end());
  if (!generated_insert_select_rows.empty()) {
    result.evidence.push_back({"insert_select_generator", "recursive_counter_cte"});
    result.evidence.push_back({"insert_select_generated_row_count",
                               std::to_string(generated_insert_select_rows.size())});
    result.evidence.insert(result.evidence.end(),
                           generated_rows_evidence.begin(),
                           generated_rows_evidence.end());
  }
  result.evidence.insert(result.evidence.end(),
                         loaded.evidence.begin(),
                         loaded.evidence.end());
  result.evidence.push_back({"relation_state_full_loads",
                             loaded.full_state_load ? "1" : "0"});
  result.evidence.push_back({"relation_state_scoped_loads",
                             loaded.scoped_state_load ? "1" : "0"});
  if (full_relation_state_required) {
    result.evidence.push_back({"relation_state_load_reason",
                               conflict_action == "do_update"
                                   ? "on_conflict_do_update_requires_child_reference_state"
                                   : "request_required_full_state"});
  } else {
    result.evidence.push_back({"relation_state_load_reason",
                               !generated_source_uuids.empty()
                                   ? "insert_select_target_source_scope"
                                   : (conflict_action == "do_update"
                                          ? "target_table_insert_and_child_reference_scope"
                                          : "target_table_insert_scope")});
  }
  if (batch_context.page_reservation.reservation_available) {
    ++result.dml_summary.page_reservations;
  }
  result.evidence.insert(result.evidence.end(),
                         route_validation.evidence.begin(),
                         route_validation.evidence.end());
  if (conflict_index) {
    result.evidence.push_back({"on_conflict_probe_path", "unique_index_lookup"});
    result.evidence.push_back({"on_conflict_delta_overlay", "statement"});
  }
  result.evidence.insert(result.evidence.end(),
                         serializable_admission.evidence.begin(),
                         serializable_admission.evidence.end());
  const bool suppress_payload_rows =
      WriteResultPolicySuppressesPayloadRows(write_result_policy);
  BulkValidationEvidenceCompactor validation_evidence;
  validation_evidence.enabled = suppress_payload_rows && input_rows.size() > 256;
  validation_evidence.input_row_count =
      static_cast<EngineApiU64>(input_rows.size());
  const bool ordinary_insert_batch_unique_preflight = conflict_action.empty();
  if (ordinary_insert_batch_unique_preflight) {
    result.evidence.push_back({"insert_unique_statement_scope",
                               "batch_key_tracker_plus_persisted_probe"});
  }
  AddMutationOptimizerEvidence("insert", request.context.local_transaction_id != 0, true, &result.evidence);
  std::vector<CrudRowVersionRecord> returning_rows;
  std::vector<StagedInsertRow> staged_insert_rows;
  InsertPreworkQueue insert_prework(request,
                                    batch_context,
                                    state,
                                    static_cast<EngineApiU64>(input_rows.size()),
                                    ordinary_insert_batch_unique_preflight);
  auto finish_insert_prework = [&]() {
    auto stats = insert_prework.Finish();
    AddInsertPreworkQueueEvidence(stats, &result.evidence);
    AddInsertPreworkAllocationResults(stats, batch_context, &result);
    return stats;
  };
  EngineApiU64 adaptive_write_window_count = 0;
  EngineApiU64 adaptive_write_window_max_rows = 0;
  EngineApiU64 adaptive_write_window_row_versions = 0;
  EngineApiU64 adaptive_write_window_index_entries = 0;
  EngineApiU64 adaptive_write_window_stream_opens = 0;
  EngineApiU64 adaptive_write_window_stream_flushes = 0;
  EngineApiU64 adaptive_write_window_scoped_stream_opens = 0;
  EngineApiU64 adaptive_write_window_scoped_stream_flushes = 0;
  EngineApiU64 adaptive_write_window_scoped_row_write_batches = 0;
  EngineApiU64 adaptive_write_window_scoped_row_write_tickets_issued = 0;
  EngineApiU64 adaptive_write_window_scoped_row_write_tickets_completed = 0;
  EngineApiU64 adaptive_write_window_scoped_row_write_worker_count = 0;
  EngineApiU64 adaptive_write_window_scoped_index_write_batches = 0;
  EngineApiU64 adaptive_write_window_scoped_index_write_tickets_issued = 0;
  EngineApiU64 adaptive_write_window_scoped_index_write_tickets_completed = 0;
  EngineApiU64 adaptive_write_window_scoped_index_write_worker_count = 0;
  EngineApiU64 adaptive_write_window_allocator_stream_opens = 0;
  EngineApiU64 adaptive_write_window_allocator_stream_flushes = 0;
  EngineApiU64 adaptive_write_window_allocator_records = 0;
  EngineApiU64 large_value_batch_windows = 0;
  EngineApiU64 large_value_batch_rows = 0;
  EngineApiU64 large_value_batch_overflows = 0;
  EngineApiU64 large_value_batch_chunks = 0;
  EngineApiU64 large_value_batch_preallocated_chunks = 0;
  EngineApiU64 large_value_batch_payload_bytes = 0;
  EngineApiU64 large_value_batch_stream_opens = 0;
  EngineApiU64 large_value_batch_stream_flushes = 0;
  UniqueStatementOverlay statement_overlay;
  const UniquePhysicalProbeCache physical_probe_cache =
      BuildUniquePhysicalProbeCache(state,
                                    request.target_table.uuid.canonical,
                                    request.context,
                                    visible_indexes);
  result.evidence.push_back({"unique_index_physical_probe_cache_entries",
                             std::to_string(physical_probe_cache.indexed_entry_count)});
  result.evidence.push_back({"unique_index_physical_probe_cache_indexes",
                             std::to_string(physical_probe_cache.index_count)});
  result.evidence.push_back({"unique_index_physical_probe_authority",
                             "candidate_only_mga_visibility_recheck_required"});
  staged_insert_rows.reserve(input_rows.size());
  for (const auto& input_row : input_rows) {
    AddInsertTrace(&batch_context, "insert.row.convert", "row", std::to_string(batch_context.actual_row_count));
    PreparedInsertRow prepared =
        PrepareInsertRowForBatch(request,
                                 input_row,
                                 batch_context.row_template,
                                 batch_context.row_encoder_plan);
    auto values = prepared.values;
    const auto default_validation =
        ApplyConstraintDefaultsForInsert(request.context, *table, values, &constraint_cache);
    if (!default_validation.ok) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", default_validation.diagnostic);
    }
    values = default_validation.values;
    validation_evidence.AppendOrCompact(default_validation.evidence,
                                        &result.evidence);
    const auto domain_validation = ApplyDomainRulesToCrudValues(request.context,
                                                                table->columns,
                                                                values,
                                                                request.context.local_transaction_id,
                                                                &constraint_cache);
    if (!domain_validation.ok) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", domain_validation.diagnostic);
    }
    values = domain_validation.values;
    validation_evidence.AppendOrCompact(domain_validation.evidence,
                                        &result.evidence);
    if (conflict_index) {
      ++result.dml_summary.index_probes;
      const auto conflict = FindUniqueConflictByIndex(state,
                                                      request.target_table.uuid.canonical,
                                                      request.context,
                                                      statement_overlay,
                                                      physical_probe_cache,
                                                      *conflict_index,
                                                      prepared.row_uuid,
                                                      values);
      if (conflict) {
        const auto& conflict_row = conflict->row;
        result.evidence.push_back({"on_conflict_action", conflict_action});
        result.evidence.push_back({"on_conflict_target", conflict_target_column});
        result.evidence.push_back({"on_conflict_match", conflict_row.row_uuid});
        result.evidence.push_back({"on_conflict_match_source", conflict->candidate_source});
        result.evidence.push_back({"on_conflict_probe_index", conflict->index_uuid});
        result.evidence.push_back({"physical_unique_index_probe_path",
                                   conflict->physical_probe_path});
        auto locator_stream = BuildOnConflictRowLocatorStream(
            request,
            request.target_table.uuid.canonical,
            conflict_row.row_uuid);
        AppendRowLocatorStreamEvidence("on_conflict", locator_stream, &result.evidence);
        if (!locator_stream.ok) {
          return InsertDiagnosticResultWithEvidence(
              request.context,
              locator_stream.diagnostic.error
                  ? locator_stream.diagnostic
                  : MakeInvalidRequestDiagnostic("dml.insert_rows",
                                                 "on_conflict_row_locator_stream_refused"),
              result.evidence);
        }
        result.evidence.push_back({"on_conflict_row_locator_stream",
                                   "consumed_row_uuid_after_unique_probe"});
        if (conflict_action == "do_nothing") {
          ++result.skipped_count;
          continue;
        }
        auto update_values = conflict_row.values;
        bool invalid_conflict_assignment_plan = false;
        const auto conflict_assignment_expressions =
            ParseInsertConflictAssignmentPlan(request, &invalid_conflict_assignment_plan);
        if (invalid_conflict_assignment_plan) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
              request.context,
              "dml.insert_rows",
              MakeInvalidRequestDiagnostic("dml.insert_rows",
                                           "on_conflict_assignment_plan_invalid"));
        }
        const auto update_columns = ConflictUpdateColumns(request, values, conflict_target_column);
        if (update_columns.empty() && conflict_assignment_expressions.empty()) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
              request.context,
              "dml.insert_rows",
              MakeInvalidRequestDiagnostic("dml.insert_rows", "on_conflict_update_columns_required"));
        }
        for (const auto& column : update_columns) {
          ReplaceValueFromExcluded(&update_values, values, column);
        }
        const auto expression_application =
            ApplyInsertConflictAssignmentExpressions(conflict_assignment_expressions,
                                                     &update_values);
        if (expression_application.error) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
              request.context,
              "dml.insert_rows",
              expression_application);
        }
        const auto update_domain_validation = ApplyDomainRulesToCrudValues(request.context,
                                                                           table->columns,
                                                                           update_values,
                                                                           request.context.local_transaction_id,
                                                                           &constraint_cache);
        if (!update_domain_validation.ok) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context,
                                                                  "dml.insert_rows",
                                                                  update_domain_validation.diagnostic);
        }
        update_values = update_domain_validation.values;
        validation_evidence.AppendOrCompact(update_domain_validation.evidence,
                                            &result.evidence);
        auto* unique_evidence =
            validation_evidence.CaptureTarget(&result.evidence);
        const auto unique_check = ValidateIndexBackedUniquePreflightForRow(
            state,
            *table,
            request.context,
            statement_overlay,
            physical_probe_cache,
            visible_indexes,
            conflict_row.row_uuid,
            update_values,
            &constraint_cache,
            unique_evidence);
        validation_evidence.FlushCapture();
        result.dml_summary.index_probes += UniqueIndexCount(visible_indexes);
        if (unique_check.error) {
          return InsertDiagnosticResultWithEvidence(request.context,
                                                    unique_check,
                                                    result.evidence);
        }
        const auto update_constraint_validation = ValidateImmediateRowConstraints(request.context,
                                                                                 state,
                                                                                 *table,
                                                                                 conflict_row.row_uuid,
                                                                                 update_values,
                                                                                 "update",
                                                                                 &constraint_cache);
        if (!update_constraint_validation.ok) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context,
                                                                  "dml.insert_rows",
                                                                  update_constraint_validation.diagnostic);
        }
        update_values = update_constraint_validation.values;
        validation_evidence.AppendOrCompact(update_constraint_validation.evidence,
                                            &result.evidence);
        const auto parent_key_update = ValidateImmediateParentKeyUpdateConstraints(request.context,
                                                                                  state,
                                                                                  *table,
                                                                                  conflict_row,
                                                                                  update_values);
        if (parent_key_update.error) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context,
                                                                  "dml.insert_rows",
                                                                  parent_key_update);
        }

        const bool update_toast_required =
            EncodedValueBytes(update_values) > kCrudVerticalSliceMaxEncodedValueBytes ||
            InsertBatchOptionEnabled(request, "large_value.force_toast=true");
        if (conflict->candidate_source == "statement_delta_overlay" &&
            ApplyStatementOverlayUpdateToStagedInsert(&staged_insert_rows,
                                                      conflict_row.row_uuid,
                                                      update_values,
                                                      update_toast_required)) {
          CrudRowVersionRecord overlay_row = conflict_row;
          overlay_row.creator_tx = request.context.local_transaction_id;
          overlay_row.table_uuid = request.target_table.uuid.canonical;
          overlay_row.deleted = false;
          overlay_row.values = update_values;
          UpsertUniqueStatementOverlayRow(&statement_overlay,
                                          visible_indexes,
                                          std::move(overlay_row));
          result.evidence.push_back({"on_conflict_update_path",
                                     "statement_delta_overlay"});
          ++result.updated_count;
          continue;
        }
        std::vector<std::pair<std::string, std::string>> storage_values = update_values;
        const std::string version_uuid = GenerateCrudEngineUuid("row");
        const auto row_allocation_start = InsertSteadyClock::now();
        const auto row_allocation = ReserveDmlPageAllocationRuntime(
            request.context,
            request.option_envelopes,
            request.target_table.uuid.canonical,
            DmlPageAllocationRuntimeFamily::row_data,
            1,
            "insert.conflict_update.row_data");
        const EngineApiU64 row_allocation_elapsed =
            ElapsedMicros(row_allocation_start, InsertSteadyClock::now());
        if (!row_allocation.ok()) {
          auto failure = AllocationFailureResult(request.context, row_allocation);
          AddDmlAllocationResourceEvidence(row_allocation,
                                           "row",
                                           row_allocation_elapsed,
                                           &failure.evidence);
          RecordDmlAllocationResourceMetrics(batch_context,
                                             row_allocation,
                                             "row",
                                             row_allocation_elapsed);
          return failure;
        }
        AddDmlPageAllocationRuntimeEvidence(row_allocation, &result);
        AddDmlAllocationSummaryCounters(row_allocation,
                                        "row",
                                        1,
                                        &result.dml_summary);
        AddDmlAllocationResourceEvidence(row_allocation,
                                         "row",
                                         row_allocation_elapsed,
                                         &result.evidence);
        if (row_allocation.active) {
          ++result.dml_summary.page_reservations;
        }
        RecordDmlAllocationResourceMetrics(batch_context,
                                           row_allocation,
                                           "row",
                                           row_allocation_elapsed);
        const auto index_allocation_start = InsertSteadyClock::now();
        const auto index_allocation = ReserveDmlIndexPageAllocationRuntime(
            request.context,
            request.option_envelopes,
            state,
            request.target_table.uuid.canonical,
            update_values,
            "insert.conflict_update.index");
        const EngineApiU64 index_allocation_elapsed =
            ElapsedMicros(index_allocation_start, InsertSteadyClock::now());
        if (!index_allocation.ok()) {
          auto failure = AllocationFailureResult(request.context, index_allocation);
          AddDmlAllocationResourceEvidence(index_allocation,
                                           "index",
                                           index_allocation_elapsed,
                                           &failure.evidence);
          RecordDmlAllocationResourceMetrics(batch_context,
                                             index_allocation,
                                             "index",
                                             index_allocation_elapsed);
          return failure;
        }
        AddDmlPageAllocationRuntimeEvidence(index_allocation, &result);
        AddDmlAllocationSummaryCounters(index_allocation,
                                        "index",
                                        0,
                                        &result.dml_summary);
        AddDmlAllocationResourceEvidence(index_allocation,
                                         "index",
                                         index_allocation_elapsed,
                                         &result.evidence);
        if (index_allocation.active) {
          ++result.dml_summary.page_reservations;
        }
        RecordDmlAllocationResourceMetrics(batch_context,
                                           index_allocation,
                                           "index",
                                           index_allocation_elapsed);
        const auto large_value_persisted = PersistMgaLargeValuesForRow(request.context,
                                                                       request.target_table.uuid.canonical,
                                                                       conflict_row.row_uuid,
                                                                       version_uuid,
                                                                       update_toast_required,
                                                                       &storage_values,
                                                                       validation_evidence.CaptureTarget(&result.evidence));
        validation_evidence.FlushCapture();
        if (large_value_persisted.error) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context,
                                                                  "dml.insert_rows",
                                                                  large_value_persisted);
        }
        CrudRowVersionRecord row_record;
        row_record.creator_tx = request.context.local_transaction_id;
        row_record.table_uuid = request.target_table.uuid.canonical;
        row_record.row_uuid = conflict_row.row_uuid;
        row_record.version_uuid = version_uuid;
        row_record.temporary_session_uuid =
            table->temporary ? request.context.session_uuid.canonical : "";
        row_record.previous_version_uuid = conflict_row.version_uuid;
        row_record.previous_sequence = conflict_row.sequence;
        row_record.deleted = false;
        row_record.values = storage_values;
        std::uint64_t written_event_sequence = 0;
        const auto appended = AppendMgaRowVersion(request.context, row_record, &written_event_sequence);
        if (appended.error) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context,
                                                                  "dml.insert_rows",
                                                                  appended);
        }
        ++result.dml_summary.append_calls;
        ++result.dml_summary.file_opens;
        ++result.dml_summary.flushes;
        const auto delta_appended = AppendMgaSecondaryIndexDeltaLedgerEntries(
            request.context,
            ConflictUpdateDeltaEntries(batch_context, conflict_row, version_uuid, update_values),
            &result.evidence);
        if (delta_appended.error) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context,
                                                                  "dml.insert_rows",
                                                                  delta_appended);
        }
        const auto synchronous_indexes = SynchronousInsertIndexes(batch_context);
        if (!synchronous_indexes.empty()) {
          ++result.dml_summary.append_calls;
          ++result.dml_summary.file_opens;
          ++result.dml_summary.flushes;
        }
        const auto index_appended = AppendMgaIndexEntriesForRowsWithIndexes(
            request.context,
            synchronous_indexes,
            request.target_table.uuid.canonical,
            std::vector<MgaIndexEntryRowInput>{{conflict_row.row_uuid, version_uuid, update_values}});
        if (index_appended.error) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context,
                                                                  "dml.insert_rows",
                                                                  index_appended);
        }
        if (index_allocation.active) {
          result.evidence.push_back({"mga_index_store", "row_update"});
        }
        CrudRowVersionRecord returning_row = conflict_row;
        returning_row.creator_tx = request.context.local_transaction_id;
        returning_row.event_sequence = written_event_sequence;
        returning_row.sequence = written_event_sequence;
        returning_row.table_uuid = request.target_table.uuid.canonical;
        returning_row.version_uuid = version_uuid;
        returning_row.previous_version_uuid = conflict_row.version_uuid;
        returning_row.previous_sequence = conflict_row.sequence;
        returning_row.deleted = false;
        returning_row.values = update_values;
        UpsertUniqueStatementOverlayRow(&statement_overlay,
                                        visible_indexes,
                                        returning_row);
        result.evidence.push_back({"on_conflict_update_path",
                                   conflict->candidate_source == "statement_delta_overlay"
                                       ? "statement_delta_overlay"
                                       : "persisted_unique_index"});
        if (!suppress_payload_rows) {
          returning_rows.push_back(std::move(returning_row));
          result.row_uuids.push_back({conflict_row.row_uuid});
        }
        ++result.updated_count;
        continue;
      }
    }
    AddInsertTrace(&batch_context, "insert.unique.preflight", "unique", prepared.row_uuid);
    const bool deferred_key_constraint = TableHasDeferredKeyConstraint(*table);
    if (deferred_key_constraint) {
      validation_evidence.PushOrCompact(
          {"constraint_deferred_unique_index_preflight",
           request.target_table.uuid.canonical},
          &result.evidence);
    } else {
      result.dml_summary.index_probes += UniqueIndexCount(visible_indexes);
      EngineApiDiagnostic unique_check =
          MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
      if (ordinary_insert_batch_unique_preflight) {
        unique_check = ValidateInsertBatchUniquePreflight(&batch_context, values);
        if (!unique_check.error) {
          for (const auto& index : visible_indexes) {
            if (!IsUniqueIndexForConflict(index)) {
              continue;
            }
            const auto keys = CrudIndexKeysForValues(index, values);
            if (const auto conflict = FindPersistedUniqueIndexConflict(
                    state,
                    request.target_table.uuid.canonical,
                    request.context,
                    physical_probe_cache,
                    index,
                    keys,
                    prepared.row_uuid)) {
              result.evidence.push_back({"insert_unique_probe_index", conflict->index_uuid});
              result.evidence.push_back({"insert_unique_probe_key", conflict->key_value});
              result.evidence.push_back({"insert_unique_probe_candidate_source",
                                         conflict->candidate_source});
              result.evidence.push_back({"physical_unique_index_probe_path",
                                         conflict->physical_probe_path});
              unique_check = UniqueConflictDiagnostic(*table, index);
              break;
            }
            RecordIndexBackedUniquePreflightProof(&constraint_cache,
                                                  request.context,
                                                  index,
                                                  prepared.row_uuid,
                                                  values,
                                                  validation_evidence.CaptureTarget(&result.evidence));
            validation_evidence.FlushCapture();
          }
        }
      } else {
        auto* unique_evidence =
            validation_evidence.CaptureTarget(&result.evidence);
        unique_check = ValidateIndexBackedUniquePreflightForRow(
            state,
            *table,
            request.context,
            statement_overlay,
            physical_probe_cache,
            visible_indexes,
            prepared.row_uuid,
            values,
            &constraint_cache,
            unique_evidence);
        validation_evidence.FlushCapture();
      }
      if (unique_check.error) {
        return InsertDiagnosticResultWithEvidence(request.context,
                                                  unique_check,
                                                  result.evidence);
      }
    }
    const auto constraint_validation = ValidateImmediateRowConstraints(request.context,
                                                                       state,
                                                                       *table,
                                                                       prepared.row_uuid,
                                                                       values,
                                                                       "insert",
                                                                       &constraint_cache);
	    if (!constraint_validation.ok) {
	      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", constraint_validation.diagnostic);
	    }
	    values = constraint_validation.values;
	    validation_evidence.AppendOrCompact(constraint_validation.evidence,
	                                        &result.evidence);
	    if (batch_context.row_encoder_plan.runtime_policy_recheck_count != 0) {
	      std::vector<EngineEvidenceReference> security_recheck_evidence;
	      const auto security_recheck =
	          EvaluateInsertRuntimeSecurityRecheck(request,
	                                               request.target_table.uuid.canonical,
	                                               values,
	                                               &security_recheck_evidence);
	      if (!security_recheck.ok || !security_recheck.admitted) {
	        result.evidence.insert(result.evidence.end(),
	                               security_recheck_evidence.begin(),
	                               security_recheck_evidence.end());
	        const EngineApiDiagnostic diagnostic =
	            security_recheck.diagnostics.empty()
	                ? MakeInvalidRequestDiagnostic("dml.insert_rows",
	                                               "runtime_security_recheck_refused")
	                : security_recheck.diagnostics.front();
	        return InsertDiagnosticResultWithEvidence(request.context,
	                                                  diagnostic,
	                                                  result.evidence);
	      }
	      validation_evidence.AppendOrCompact(security_recheck_evidence,
	                                          &result.evidence);
	    }
	    prepared.values = values;
    prepared.encoded_bytes = static_cast<EngineApiU64>(EncodedValueBytes(values));
    prepared.toast_required = prepared.encoded_bytes > batch_context.row_template.max_inline_encoded_bytes ||
                              InsertBatchOptionEnabled(request, "large_value.force_toast=true");
    const auto memory_validation = ValidateInsertBatchMemoryBudget(
        batch_context,
        prepared.toast_required ? batch_context.row_template.max_inline_encoded_bytes : prepared.encoded_bytes);
    if (memory_validation.error) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", memory_validation);
    }
    const auto constraint_check = ValidateInsertBatchConstraints(batch_context, state, prepared);
    if (constraint_check.error) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", constraint_check);
    }
    const std::string version_uuid = GenerateCrudEngineUuid("row");
    CrudRowVersionRecord row_record;
    row_record.creator_tx = request.context.local_transaction_id;
    row_record.table_uuid = request.target_table.uuid.canonical;
    row_record.row_uuid = prepared.row_uuid;
    row_record.version_uuid = version_uuid;
    row_record.temporary_session_uuid =
        table->temporary ? request.context.session_uuid.canonical : "";
    row_record.deleted = false;
    row_record.values = values;
    CrudRowVersionRecord overlay_row = row_record;
    AddInsertTrace(&batch_context, "insert.row.stage", "stage", prepared.row_uuid);
    staged_insert_rows.push_back({std::move(row_record), values, prepared.toast_required});
    if (!insert_prework.Enqueue(values, prepared.encoded_bytes)) {
      const auto prework_stats = finish_insert_prework();
      const EngineApiDiagnostic diagnostic =
          prework_stats.failed
              ? prework_stats.diagnostic
              : MakeInvalidRequestDiagnostic("dml.insert_rows",
                                             "insert_prework_queue_stopped");
      return InsertDiagnosticResultWithEvidence(request.context,
                                                diagnostic,
                                                result.evidence);
    }
    if (!ordinary_insert_batch_unique_preflight) {
      UpsertUniqueStatementOverlayRow(&statement_overlay,
                                      visible_indexes,
                                      std::move(overlay_row));
    }
  }

  if (!staged_insert_rows.empty()) {
    auto serializable_recorded = dml::RecordSerializableInsertMutation(
        request.context,
        "dml.insert_rows",
        request.target_table.uuid.canonical,
        input_rows,
        request.option_envelopes);
    if (!serializable_recorded.ok) {
      auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
          request.context,
          "dml.insert_rows",
          serializable_recorded.diagnostic);
      const auto prework_stats = finish_insert_prework();
      (void)prework_stats;
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
    const auto prework_stats = finish_insert_prework();
    mark_insert_phase("insert_prework_queue_fence");
    if (prework_stats.failed) {
      return InsertDiagnosticResultWithEvidence(request.context,
                                                prework_stats.diagnostic,
                                                result.evidence);
    }

    const EngineApiU64 admitted_rows =
        std::max<EngineApiU64>(1, batch_context.adaptive_batch_plan.admitted_rows);
    const bool prework_row_capacity_ready =
        prework_stats.row_prework_rows >=
        static_cast<EngineApiU64>(staged_insert_rows.size());
    result.evidence.push_back({"insert_prework_row_capacity_ready",
                               prework_row_capacity_ready ? "true" : "false"});
    if (!prework_row_capacity_ready) {
      const auto row_allocation_start = InsertSteadyClock::now();
      const auto row_allocation = ReserveDmlPageAllocationRuntime(
          request.context,
          request.option_envelopes,
          request.target_table.uuid.canonical,
          DmlPageAllocationRuntimeFamily::row_data,
          admitted_rows,
          "insert.row_data");
      const EngineApiU64 row_allocation_elapsed =
          ElapsedMicros(row_allocation_start, InsertSteadyClock::now());
      if (!row_allocation.ok()) {
        auto failure = AllocationFailureResult(request.context, row_allocation);
        AddDmlAllocationResourceEvidence(row_allocation,
                                         "row",
                                         row_allocation_elapsed,
                                         &failure.evidence);
        RecordDmlAllocationResourceMetrics(batch_context,
                                           row_allocation,
                                           "row",
                                           row_allocation_elapsed);
        return failure;
      }
      AddDmlPageAllocationRuntimeEvidence(row_allocation, &result);
      AddDmlAllocationSummaryCounters(row_allocation,
                                      "row",
                                      admitted_rows,
                                      &result.dml_summary);
      AddDmlAllocationResourceEvidence(row_allocation,
                                       "row",
                                       row_allocation_elapsed,
                                       &result.evidence);
      if (row_allocation.active) {
        ++result.dml_summary.page_reservations;
      }
      RecordDmlAllocationResourceMetrics(batch_context,
                                         row_allocation,
                                         "row",
                                         row_allocation_elapsed);
    }

    EngineApiU64 preworked_index_rows_remaining =
        prework_stats.index_prework_rows;
    for (std::size_t window_begin = 0; window_begin < staged_insert_rows.size();) {
      const std::size_t window_size = static_cast<std::size_t>(
          std::min<EngineApiU64>(
              admitted_rows,
              static_cast<EngineApiU64>(staged_insert_rows.size() - window_begin)));
      const std::size_t window_end = window_begin + window_size;
      ++adaptive_write_window_count;
      adaptive_write_window_max_rows =
          std::max<EngineApiU64>(adaptive_write_window_max_rows,
                                 static_cast<EngineApiU64>(window_size));
      AddInsertTrace(&batch_context,
                     "insert.adaptive_write_window",
                     "write",
                     std::to_string(window_size));

      std::vector<std::vector<std::pair<std::string, std::string>>> index_value_batch;
      index_value_batch.reserve(window_size);
      for (std::size_t index = window_begin; index < window_end; ++index) {
        index_value_batch.push_back(staged_insert_rows[index].logical_values);
      }
      DmlPageAllocationRuntimeResult index_allocation;
      bool prework_index_capacity_ready = false;
      if (preworked_index_rows_remaining >= static_cast<EngineApiU64>(window_size)) {
        prework_index_capacity_ready = true;
        preworked_index_rows_remaining -= static_cast<EngineApiU64>(window_size);
      }
      result.evidence.push_back({"insert_prework_index_window_capacity_ready",
                                 prework_index_capacity_ready ? "true" : "false"});
      if (!prework_index_capacity_ready) {
        const auto index_allocation_start = InsertSteadyClock::now();
        index_allocation = ReserveDmlIndexPageAllocationRuntimeForRows(
            request.context,
            request.option_envelopes,
            state,
            request.target_table.uuid.canonical,
            index_value_batch,
            "insert.index");
        const EngineApiU64 index_allocation_elapsed =
            ElapsedMicros(index_allocation_start, InsertSteadyClock::now());
        if (!index_allocation.ok()) {
          auto failure = AllocationFailureResult(request.context, index_allocation);
          AddDmlAllocationResourceEvidence(index_allocation,
                                           "index",
                                           index_allocation_elapsed,
                                           &failure.evidence);
          RecordDmlAllocationResourceMetrics(batch_context,
                                             index_allocation,
                                             "index",
                                             index_allocation_elapsed);
          return failure;
        }
        AddDmlPageAllocationRuntimeEvidence(index_allocation, &result);
        AddDmlAllocationSummaryCounters(index_allocation,
                                        "index",
                                        0,
                                        &result.dml_summary);
        AddDmlAllocationResourceEvidence(index_allocation,
                                         "index",
                                         index_allocation_elapsed,
                                         &result.evidence);
        if (index_allocation.active) {
          ++result.dml_summary.page_reservations;
        }
        RecordDmlAllocationResourceMetrics(batch_context,
                                           index_allocation,
                                           "index",
                                           index_allocation_elapsed);
      }

      std::vector<CrudRowVersionRecord> row_records;
      row_records.reserve(window_size);
      std::vector<std::vector<std::pair<std::string, std::string>>> storage_value_batch;
      storage_value_batch.reserve(window_size);
      for (std::size_t index = window_begin; index < window_end; ++index) {
        auto& staged = staged_insert_rows[index];
        storage_value_batch.push_back(staged.logical_values);
      }
      std::vector<MgaLargeValuePersistBatchRowInput> large_value_rows;
      large_value_rows.reserve(window_size);
      for (std::size_t index = window_begin; index < window_end; ++index) {
        auto& staged = staged_insert_rows[index];
        large_value_rows.push_back(
            MgaLargeValuePersistBatchRowInput{request.target_table.uuid.canonical,
                                              staged.row_record.row_uuid,
                                              staged.row_record.version_uuid,
                                              staged.toast_required,
                                              &storage_value_batch[index - window_begin]});
      }
      MgaLargeValuePersistBatchCounters large_value_counters;
      const auto large_value_persisted =
          PersistMgaLargeValuesForRows(request.context,
                                       large_value_rows,
                                       &large_value_counters,
                                       validation_evidence.CaptureTarget(&result.evidence));
      validation_evidence.FlushCapture();
      if (large_value_persisted.error) {
        return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", large_value_persisted);
      }
      if (large_value_counters.values_overflowed != 0) {
        ++large_value_batch_windows;
      }
      large_value_batch_rows += large_value_counters.rows_seen;
      large_value_batch_overflows += large_value_counters.values_overflowed;
      large_value_batch_chunks += large_value_counters.chunks_appended;
      large_value_batch_preallocated_chunks +=
          large_value_counters.preallocated_chunk_slots;
      large_value_batch_payload_bytes += large_value_counters.payload_bytes;
      large_value_batch_stream_opens += large_value_counters.stream_opens;
      large_value_batch_stream_flushes += large_value_counters.stream_flushes;
      for (std::size_t index = window_begin; index < window_end; ++index) {
        auto& staged = staged_insert_rows[index];
        staged.row_record.values = std::move(storage_value_batch[index - window_begin]);
        row_records.push_back(staged.row_record);
      }

      std::vector<std::uint64_t> written_event_sequences;
      MgaRelationHotAppendContext hot_append(request.context);
      if (IparFaultPointRequested(request.option_envelopes, "row_append")) {
        std::vector<EngineEvidenceReference> evidence = result.evidence;
        AppendIparFaultEvidence(&evidence,
                                "row_append",
                                "rollback_required_before_row_append");
        return InsertDiagnosticResultWithEvidence(
            request.context,
            IparFaultDiagnostic("dml.insert_rows", "row_append", "phase=row_append"),
            evidence);
      }
      const auto appended =
          hot_append.AppendRowVersions(&row_records, &written_event_sequences);
      if (appended.error) {
        return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", appended);
      }
      const auto rows_flushed = hot_append.FlushRowVersions();
      if (rows_flushed.error) {
        return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", rows_flushed);
      }
      if (IparFaultPointRequested(request.option_envelopes, "index_append")) {
        std::vector<EngineEvidenceReference> evidence = result.evidence;
        AppendIparFaultEvidence(&evidence,
                                "index_append",
                                "rollback_required_after_row_append_before_index_append");
        evidence.push_back({"ipar_fault_injection_row_versions_staged",
                            std::to_string(row_records.size())});
        return InsertDiagnosticResultWithEvidence(
            request.context,
            IparFaultDiagnostic("dml.insert_rows", "index_append", "phase=index_append"),
            evidence);
      }

      std::vector<MgaIndexEntryRowInput> index_rows;
      index_rows.reserve(window_size);
      std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> delta_entries;
      for (std::size_t index = window_begin; index < window_end; ++index) {
        const auto& row_record = row_records[index - window_begin];
        AddInsertTrace(&batch_context, "insert.row.write", "write", row_record.row_uuid);
        AddInsertTrace(&batch_context, "insert.index.maintain", "index", row_record.row_uuid);
        index_rows.push_back({row_record.row_uuid,
                              row_record.version_uuid,
                              staged_insert_rows[index].logical_values});
        auto row_delta_entries = InsertDeltaEntries(batch_context,
                                                    row_record,
                                                    staged_insert_rows[index].logical_values);
        delta_entries.insert(delta_entries.end(),
                             std::make_move_iterator(row_delta_entries.begin()),
                             std::make_move_iterator(row_delta_entries.end()));
      }
      const auto delta_appended = AppendMgaSecondaryIndexDeltaLedgerEntries(
          request.context,
          delta_entries,
          &result.evidence);
      if (delta_appended.error) {
        return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", delta_appended);
      }
      const auto synchronous_indexes = SynchronousInsertIndexes(batch_context);
      if (!delta_entries.empty()) {
        ++result.dml_summary.append_calls;
      }
      std::vector<MgaIndexEntryAppendBatch> index_batches;
      index_batches.reserve(synchronous_indexes.size());
      for (const auto& index : synchronous_indexes) {
        MgaIndexEntryAppendBatch batch;
        batch.index = index;
        batch.table_uuid = request.target_table.uuid.canonical;
        batch.rows = index_rows;
        index_batches.push_back(std::move(batch));
      }
      const auto index_appended = hot_append.AppendIndexEntryBatches(index_batches);
      if (index_appended.error) {
        return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", index_appended);
      }
      const auto index_flushed = hot_append.FlushIndexEntries();
      if (index_flushed.error) {
        return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", index_flushed);
      }
      if (index_allocation.active || prework_index_capacity_ready) {
        result.evidence.push_back({"mga_index_store", "row_insert"});
      }
      const auto hot_counters = hot_append.counters();
      adaptive_write_window_row_versions += hot_counters.row_versions_appended;
      adaptive_write_window_index_entries += hot_counters.index_entries_appended;
      adaptive_write_window_stream_opens +=
          hot_counters.row_stream_opens + hot_counters.index_stream_opens;
      adaptive_write_window_stream_flushes +=
          hot_counters.row_stream_flushes + hot_counters.index_stream_flushes;
      adaptive_write_window_scoped_stream_opens +=
          hot_counters.scoped_row_stream_opens +
          hot_counters.scoped_index_stream_opens;
      adaptive_write_window_scoped_stream_flushes +=
          hot_counters.scoped_row_stream_flushes +
          hot_counters.scoped_index_stream_flushes;
      adaptive_write_window_scoped_row_write_batches +=
          hot_counters.scoped_row_write_batches;
      adaptive_write_window_scoped_row_write_tickets_issued +=
          hot_counters.scoped_row_write_tickets_issued;
      adaptive_write_window_scoped_row_write_tickets_completed +=
          hot_counters.scoped_row_write_tickets_completed;
      adaptive_write_window_scoped_row_write_worker_count =
          std::max(adaptive_write_window_scoped_row_write_worker_count,
                   hot_counters.scoped_row_write_worker_count);
      adaptive_write_window_scoped_index_write_batches +=
          hot_counters.scoped_index_write_batches;
      adaptive_write_window_scoped_index_write_tickets_issued +=
          hot_counters.scoped_index_write_tickets_issued;
      adaptive_write_window_scoped_index_write_tickets_completed +=
          hot_counters.scoped_index_write_tickets_completed;
      adaptive_write_window_scoped_index_write_worker_count =
          std::max(adaptive_write_window_scoped_index_write_worker_count,
                   hot_counters.scoped_index_write_worker_count);
      adaptive_write_window_allocator_stream_opens +=
          hot_counters.allocator_stream_opens;
      adaptive_write_window_allocator_stream_flushes +=
          hot_counters.allocator_stream_flushes;
      adaptive_write_window_allocator_records +=
          hot_counters.allocator_range_records_appended;
      if (hot_counters.row_versions_appended != 0) {
        ++result.dml_summary.append_calls;
      }
      if (hot_counters.index_entries_appended != 0) {
        ++result.dml_summary.append_calls;
      }
      result.dml_summary.file_opens +=
          hot_counters.row_stream_opens + hot_counters.index_stream_opens +
          hot_counters.scoped_row_stream_opens +
          hot_counters.scoped_index_stream_opens +
          hot_counters.allocator_stream_opens;
      result.dml_summary.flushes +=
          hot_counters.row_stream_flushes + hot_counters.index_stream_flushes +
          hot_counters.scoped_row_stream_flushes +
          hot_counters.scoped_index_stream_flushes +
          hot_counters.allocator_stream_flushes;

      for (std::size_t index = window_begin; index < window_end; ++index) {
        const auto& row_record = row_records[index - window_begin];
        if (!suppress_payload_rows) {
          result.row_uuids.push_back({row_record.row_uuid});
          CrudRowVersionRecord returning_row;
          returning_row.creator_tx = request.context.local_transaction_id;
          returning_row.event_sequence = row_record.event_sequence;
          returning_row.sequence = row_record.sequence;
          returning_row.table_uuid = request.target_table.uuid.canonical;
          returning_row.row_uuid = row_record.row_uuid;
          returning_row.version_uuid = row_record.version_uuid;
          returning_row.deleted = false;
          returning_row.values = staged_insert_rows[index].logical_values;
          returning_rows.push_back(std::move(returning_row));
        }
        ++result.inserted_count;
        ++batch_context.actual_row_count;
      }
      window_begin = window_end;
    }

    result.evidence.push_back({"insert_adaptive_write_window_count",
                               std::to_string(adaptive_write_window_count)});
    result.evidence.push_back({"insert_adaptive_write_window_max_rows",
                               std::to_string(adaptive_write_window_max_rows)});
    result.evidence.push_back({"insert_hot_append_row_versions",
                               std::to_string(adaptive_write_window_row_versions)});
    result.evidence.push_back({"insert_hot_append_index_entries",
                               std::to_string(adaptive_write_window_index_entries)});
    result.evidence.push_back({"insert_hot_append_stream_opens",
                               std::to_string(adaptive_write_window_stream_opens)});
    result.evidence.push_back({"insert_hot_append_stream_flushes",
                               std::to_string(adaptive_write_window_stream_flushes)});
    result.evidence.push_back({"insert_hot_append_scoped_stream_opens",
                               std::to_string(adaptive_write_window_scoped_stream_opens)});
    result.evidence.push_back({"insert_hot_append_scoped_stream_flushes",
                               std::to_string(adaptive_write_window_scoped_stream_flushes)});
    result.evidence.push_back({"insert_hot_append_scoped_row_write_batches",
                               std::to_string(adaptive_write_window_scoped_row_write_batches)});
    result.evidence.push_back({"insert_hot_append_scoped_row_write_tickets_issued",
                               std::to_string(adaptive_write_window_scoped_row_write_tickets_issued)});
    result.evidence.push_back({"insert_hot_append_scoped_row_write_tickets_completed",
                               std::to_string(adaptive_write_window_scoped_row_write_tickets_completed)});
    result.evidence.push_back({"insert_hot_append_scoped_row_write_worker_count",
                               std::to_string(adaptive_write_window_scoped_row_write_worker_count)});
    result.evidence.push_back({"insert_hot_append_scoped_index_write_batches",
                               std::to_string(adaptive_write_window_scoped_index_write_batches)});
    result.evidence.push_back({"insert_hot_append_scoped_index_write_tickets_issued",
                               std::to_string(adaptive_write_window_scoped_index_write_tickets_issued)});
    result.evidence.push_back({"insert_hot_append_scoped_index_write_tickets_completed",
                               std::to_string(adaptive_write_window_scoped_index_write_tickets_completed)});
    result.evidence.push_back({"insert_hot_append_scoped_index_write_worker_count",
                               std::to_string(adaptive_write_window_scoped_index_write_worker_count)});
    result.evidence.push_back({"insert_hot_append_allocator_stream_opens",
                               std::to_string(adaptive_write_window_allocator_stream_opens)});
    result.evidence.push_back({"insert_hot_append_allocator_stream_flushes",
                               std::to_string(adaptive_write_window_allocator_stream_flushes)});
    result.evidence.push_back({"insert_hot_append_allocator_records",
                               std::to_string(adaptive_write_window_allocator_records)});
    result.evidence.push_back({"insert_large_value_batch_windows",
                               std::to_string(large_value_batch_windows)});
    result.evidence.push_back({"insert_large_value_batch_rows",
                               std::to_string(large_value_batch_rows)});
    result.evidence.push_back({"insert_large_value_batch_overflows",
                               std::to_string(large_value_batch_overflows)});
    result.evidence.push_back({"insert_large_value_batch_chunks",
                               std::to_string(large_value_batch_chunks)});
    result.evidence.push_back({"insert_large_value_batch_preallocated_chunks",
                               std::to_string(large_value_batch_preallocated_chunks)});
    result.evidence.push_back({"insert_large_value_batch_payload_bytes",
                               std::to_string(large_value_batch_payload_bytes)});
    result.evidence.push_back({"insert_large_value_batch_stream_opens",
                               std::to_string(large_value_batch_stream_opens)});
    result.evidence.push_back({"insert_large_value_batch_stream_flushes",
                               std::to_string(large_value_batch_stream_flushes)});
  }
  AddInsertTrace(&batch_context, "insert.batch.finish", "finish", std::to_string(batch_context.actual_row_count));
  if (suppress_payload_rows) {
    result.result_shape.result_kind = "dml_insert_result_suppressed";
  } else {
    result.result_shape = CrudRowsToResultShape(returning_rows);
  }
  if (result.inserted_count != 0) {
    result.evidence.push_back({"mga_row_version", "row_insert"});
    result.evidence.push_back({"mga_row_store", "row_insert"});
  }
  if (result.updated_count != 0) {
    result.evidence.push_back({"mga_row_version", "row_update"});
    result.evidence.push_back({"mga_row_store", "row_update"});
  }
  if (result.skipped_count != 0) {
    result.evidence.push_back({"mga_row_store", "row_conflict_skipped"});
  }
  result.evidence.push_back({"domain_validation", "write_path_checked"});
  result.evidence.push_back({"relation_descriptor", relation_descriptor.descriptor_uuid.canonical});
  result.evidence.push_back({"dml_returning", "affected_rows"});
  result.evidence.push_back({"row_uuid_generation", request.require_generated_row_uuid ? "required" : "caller_allowed"});
  result.evidence.push_back({"trigger_udr_hooks", "inactive_checked"});
  result.evidence.push_back({"unique_index_logical_preflight_probes",
                             std::to_string(result.dml_summary.index_probes)});
  result.evidence.push_back({"unique_index_physical_probes",
                             std::to_string(physical_probe_cache.physical_probe_attempts)});
  result.evidence.push_back({"unique_index_physical_probe_hits",
                             std::to_string(physical_probe_cache.physical_probe_hits)});
  result.evidence.push_back({"unique_index_physical_probe_misses",
                             std::to_string(physical_probe_cache.physical_probe_misses)});
  result.evidence.push_back({"unique_index_scan_fallbacks",
                             std::to_string(physical_probe_cache.scan_fallback_attempts)});
  result.evidence.push_back({"unique_index_scan_fallback_hits",
                             std::to_string(physical_probe_cache.scan_fallback_hits)});
  result.evidence.push_back({"prepared_descriptor_hits",
                             batch_context.prepared_descriptor_cache_hit ? "1" : "0"});
  result.evidence.push_back({"prepared_descriptor_misses",
                             batch_context.prepared_descriptor_cache_hit ? "0" : "1"});
  validation_evidence.AddSummaryEvidence(&result.evidence);
  AddInsertBatchEvidenceToResult(batch_context, &result);
  if (!batch_context.fallback_reason.empty()) {
    AddDmlSummaryFallbackReason(&result.dml_summary, batch_context.fallback_reason);
  }
  result.dml_summary.rows_changed = result.inserted_count + result.updated_count;
  AddDmlSummaryEvidence(&result);
  ApplyWriteResultPolicy(write_result_policy, &result);
  RecordInsertBatchMetric(batch_context, "sb_dml_insert_batch_started_total", 1.0, "ok");
  if (physical_probe_cache.physical_probe_attempts != 0) {
    RecordInsertBatchMetric(batch_context,
                            "sb_index_insert_unique_physical_probe_total",
                            static_cast<double>(physical_probe_cache.physical_probe_attempts),
                            "physical_probe",
                            "mga_visibility_rechecked");
  }
  if (physical_probe_cache.scan_fallback_attempts != 0) {
    RecordInsertBatchMetric(batch_context,
                            "sb_index_insert_unique_physical_probe_total",
                            static_cast<double>(physical_probe_cache.scan_fallback_attempts),
                            "scan_fallback",
                            "physical_probe_cache_miss");
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_slow_path_total",
                            static_cast<double>(physical_probe_cache.scan_fallback_attempts),
                            "scan_fallback",
                            "unique_physical_probe_cache_miss");
  }
  if (full_relation_state_required) {
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_slow_path_total",
                            1.0,
                            "full_relation_state",
                            conflict_action == "do_update"
                                ? "on_conflict_do_update_requires_child_reference_state"
                                : "request_required_full_state");
  }
  if (batch_context.adaptive_batch_plan.reduced) {
    RecordInsertBatchMetric(batch_context,
                            "sb_dml_insert_slow_path_total",
                            1.0,
                            "adaptive_batch_reduced",
                            batch_context.adaptive_batch_plan.reason);
  }
  RecordInsertBatchMetric(batch_context, "sb_dml_insert_rows_inserted_total", static_cast<double>(result.inserted_count), "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api
