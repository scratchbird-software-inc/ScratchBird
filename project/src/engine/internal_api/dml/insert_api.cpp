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
#include "dml/dml_row_locator_stream.hpp"
#include "dml/page_allocation_runtime_bridge.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "dml/write_result_policy.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "observability/dml_summary_counters.hpp"
#include "physical_plan.hpp"
#include "relational_planner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

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
        StartsWith(envelope, "include:") || StartsWith(envelope, "where_eq:")) {
      continue;
    }
    columns.push_back(StartsWith(envelope, "identity:") ? envelope.substr(9) : envelope);
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
  for (const auto& [field, ignored] : values) {
    (void)ignored;
    if (field != target_column) { option_columns.push_back(field); }
  }
  return option_columns;
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
};

struct UniquePreflightRouteValidation {
  bool ok = true;
  EngineApiDiagnostic diagnostic;
  std::vector<EngineEvidenceReference> evidence;
};

struct UniqueStatementOverlay {
  std::map<std::string, CrudRowVersionRecord> rows_by_uuid;
  std::map<std::string, std::map<std::string, std::set<std::string>>> key_rows_by_index_uuid;
};

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
                                       "statement_delta_overlay"};
    }
  }
  return std::nullopt;
}

std::optional<UniqueConflictProbeResult> FindPersistedUniqueIndexConflict(
    const CrudState& state,
    const std::string& table_uuid,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    const std::vector<std::string>& keys,
    const std::string& exclude_row_uuid) {
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
        return UniqueConflictProbeResult{*row,
                                         index.index_uuid,
                                         key,
                                         "persisted_unique_index"};
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
                                          index,
                                          keys,
                                          exclude_row_uuid);
}

EngineApiDiagnostic ValidateIndexBackedUniquePreflightForRow(
    const CrudState& state,
    const CrudTableRecord& table,
    const EngineRequestContext& context,
    const UniqueStatementOverlay& overlay,
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
                                                    index,
                                                    row_uuid,
                                                    values);
    if (conflict) {
      if (evidence != nullptr) {
        evidence->push_back({"insert_unique_probe_index", conflict->index_uuid});
        evidence->push_back({"insert_unique_probe_key", conflict->key_value});
        evidence->push_back({"insert_unique_probe_candidate_source",
                             conflict->candidate_source});
        evidence->push_back({"insert_unique_mga_recheck", "row_uuid_candidate"});
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
  const std::span<const EngineRowValue> input_rows = request.EffectiveInputRows();
  if (input_rows.empty()) {
    return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", MakeInvalidRequestDiagnostic("dml.insert_rows", "at_least_one_row_required"));
  }
  const auto write_result_policy =
      ResolveWriteResultPolicy(request, "dml.insert_rows");
  if (!write_result_policy.ok) {
    auto failure = MakeCrudDiagnosticResult<EngineInsertRowsResult>(
        request.context,
        "dml.insert_rows",
        write_result_policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  const auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", loaded.diagnostic); }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindVisibleCrudTable(state, request.target_table.uuid.canonical, request.context.local_transaction_id);
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
  if (CrudRowsTouchOpaqueColumn(*table, input_rows)) {
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
  const std::string conflict_action = ConflictAction(request);
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
  auto result = MakeCrudSuccessResult<EngineInsertRowsResult>(request.context, "dml.insert_rows");
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
  AddMutationOptimizerEvidence("insert", request.context.local_transaction_id != 0, true, &result.evidence);
  std::vector<CrudRowVersionRecord> returning_rows;
  std::vector<StagedInsertRow> staged_insert_rows;
  UniqueStatementOverlay statement_overlay;
  staged_insert_rows.reserve(input_rows.size());
  for (const auto& input_row : input_rows) {
    AddInsertTrace(&batch_context, "insert.row.convert", "row", std::to_string(batch_context.actual_row_count));
    PreparedInsertRow prepared = PrepareInsertRowForBatch(request, input_row, batch_context.row_template);
    auto values = prepared.values;
    const auto default_validation = ApplyConstraintDefaultsForInsert(request.context, *table, values);
    if (!default_validation.ok) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", default_validation.diagnostic);
    }
    values = default_validation.values;
    for (const auto& evidence : default_validation.evidence) { result.evidence.push_back(evidence); }
    const auto domain_validation = ApplyDomainRulesToCrudValues(request.context,
                                                                table->columns,
                                                                values,
                                                                request.context.local_transaction_id,
                                                                &constraint_cache);
    if (!domain_validation.ok) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", domain_validation.diagnostic);
    }
    values = domain_validation.values;
    for (const auto& evidence : domain_validation.evidence) {
      result.evidence.push_back(evidence);
    }
    if (conflict_index) {
      ++result.dml_summary.index_probes;
      const auto conflict = FindUniqueConflictByIndex(state,
                                                      request.target_table.uuid.canonical,
                                                      request.context,
                                                      statement_overlay,
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
        result.evidence.push_back({"physical_index_tree_available", "false"});
        result.evidence.push_back({"irc060_required_for_physical_scan", "true"});
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
        const auto update_columns = ConflictUpdateColumns(request, values, conflict_target_column);
        if (update_columns.empty()) {
          return MakeCrudDiagnosticResult<EngineInsertRowsResult>(
              request.context,
              "dml.insert_rows",
              MakeInvalidRequestDiagnostic("dml.insert_rows", "on_conflict_update_columns_required"));
        }
        for (const auto& column : update_columns) {
          ReplaceValueFromExcluded(&update_values, values, column);
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
        for (const auto& evidence : update_domain_validation.evidence) {
          result.evidence.push_back(evidence);
        }
        const auto unique_check = ValidateIndexBackedUniquePreflightForRow(
            state,
            *table,
            request.context,
            statement_overlay,
            visible_indexes,
            conflict_row.row_uuid,
            update_values,
            &constraint_cache,
            &result.evidence);
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
        for (const auto& evidence : update_constraint_validation.evidence) {
          result.evidence.push_back(evidence);
        }
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
        const auto row_allocation = ReserveDmlPageAllocationRuntime(
            request.context,
            request.option_envelopes,
            request.target_table.uuid.canonical,
            DmlPageAllocationRuntimeFamily::row_data,
            1,
            "insert.conflict_update.row_data");
        if (!row_allocation.ok()) {
          return AllocationFailureResult(request.context, row_allocation);
        }
        AddDmlPageAllocationRuntimeEvidence(row_allocation, &result);
        if (row_allocation.active) {
          ++result.dml_summary.page_reservations;
        }
        const auto index_allocation = ReserveDmlIndexPageAllocationRuntime(
            request.context,
            request.option_envelopes,
            state,
            request.target_table.uuid.canonical,
            update_values,
            "insert.conflict_update.index");
        if (!index_allocation.ok()) {
          return AllocationFailureResult(request.context, index_allocation);
        }
        AddDmlPageAllocationRuntimeEvidence(index_allocation, &result);
        if (index_allocation.active) {
          ++result.dml_summary.page_reservations;
        }
        const auto large_value_persisted = PersistMgaLargeValuesForRow(request.context,
                                                                       request.target_table.uuid.canonical,
                                                                       conflict_row.row_uuid,
                                                                       version_uuid,
                                                                       update_toast_required,
                                                                       &storage_values,
                                                                       &result.evidence);
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
      result.evidence.push_back({"constraint_deferred_unique_index_preflight", request.target_table.uuid.canonical});
    } else {
      result.dml_summary.index_probes += UniqueIndexCount(visible_indexes);
      const auto unique_check = ValidateIndexBackedUniquePreflightForRow(
          state,
          *table,
          request.context,
          statement_overlay,
          visible_indexes,
          prepared.row_uuid,
          values,
          &constraint_cache,
          &result.evidence);
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
    for (const auto& evidence : constraint_validation.evidence) { result.evidence.push_back(evidence); }
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
    UpsertUniqueStatementOverlayRow(&statement_overlay,
                                    visible_indexes,
                                    std::move(overlay_row));
  }

  if (!staged_insert_rows.empty()) {
    const auto row_allocation = ReserveDmlPageAllocationRuntime(
        request.context,
        request.option_envelopes,
        request.target_table.uuid.canonical,
        DmlPageAllocationRuntimeFamily::row_data,
        static_cast<std::uint64_t>(staged_insert_rows.size()),
        "insert.row_data");
    if (!row_allocation.ok()) {
      return AllocationFailureResult(request.context, row_allocation);
    }
    AddDmlPageAllocationRuntimeEvidence(row_allocation, &result);
    if (row_allocation.active) {
      ++result.dml_summary.page_reservations;
    }

    std::vector<std::vector<std::pair<std::string, std::string>>> index_value_batch;
    index_value_batch.reserve(staged_insert_rows.size());
    for (const auto& staged : staged_insert_rows) {
      index_value_batch.push_back(staged.logical_values);
    }
    const auto index_allocation = ReserveDmlIndexPageAllocationRuntimeForRows(
        request.context,
        request.option_envelopes,
        state,
        request.target_table.uuid.canonical,
        index_value_batch,
        "insert.index");
    if (!index_allocation.ok()) {
      return AllocationFailureResult(request.context, index_allocation);
    }
    AddDmlPageAllocationRuntimeEvidence(index_allocation, &result);
    if (index_allocation.active) {
      ++result.dml_summary.page_reservations;
    }

    std::vector<CrudRowVersionRecord> row_records;
    row_records.reserve(staged_insert_rows.size());
    for (auto& staged : staged_insert_rows) {
      auto storage_values = staged.logical_values;
      const auto large_value_persisted = PersistMgaLargeValuesForRow(request.context,
                                                                     request.target_table.uuid.canonical,
                                                                     staged.row_record.row_uuid,
                                                                     staged.row_record.version_uuid,
                                                                     staged.toast_required,
                                                                     &storage_values,
                                                                     &result.evidence);
      if (large_value_persisted.error) {
        return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", large_value_persisted);
      }
      staged.row_record.values = std::move(storage_values);
      row_records.push_back(staged.row_record);
    }

    std::vector<std::uint64_t> written_event_sequences;
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
    const auto appended = AppendMgaRowVersions(request.context, &row_records, &written_event_sequences);
    if (appended.error) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", appended);
    }
    ++result.dml_summary.append_calls;
    ++result.dml_summary.file_opens;
    ++result.dml_summary.flushes;

    std::vector<MgaIndexEntryRowInput> index_rows;
    index_rows.reserve(staged_insert_rows.size());
    std::vector<MgaSecondaryIndexDeltaLedgerEntryInput> delta_entries;
    for (std::size_t index = 0; index < staged_insert_rows.size(); ++index) {
      const auto& row_record = row_records[index];
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
    if (!synchronous_indexes.empty() && !index_rows.empty()) {
      ++result.dml_summary.append_calls;
      ++result.dml_summary.file_opens;
      ++result.dml_summary.flushes;
    }
    if (!delta_entries.empty()) {
      ++result.dml_summary.append_calls;
    }
    const auto index_appended = AppendMgaIndexEntriesForRowsWithIndexes(request.context,
                                                                        synchronous_indexes,
                                                                        request.target_table.uuid.canonical,
                                                                        index_rows);
    if (index_appended.error) {
      return MakeCrudDiagnosticResult<EngineInsertRowsResult>(request.context, "dml.insert_rows", index_appended);
    }
    if (index_allocation.active) {
      result.evidence.push_back({"mga_index_store", "row_insert"});
    }

    for (std::size_t index = 0; index < staged_insert_rows.size(); ++index) {
      const auto& row_record = row_records[index];
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
  AddInsertBatchEvidenceToResult(batch_context, &result);
  if (!batch_context.fallback_reason.empty()) {
    AddDmlSummaryFallbackReason(&result.dml_summary, batch_context.fallback_reason);
  }
  result.dml_summary.rows_changed = result.inserted_count + result.updated_count;
  AddDmlSummaryEvidence(&result);
  ApplyWriteResultPolicy(write_result_policy, &result);
  RecordInsertBatchMetric(batch_context, "sb_dml_insert_batch_started_total", 1.0, "ok");
  RecordInsertBatchMetric(batch_context, "sb_dml_insert_rows_inserted_total", static_cast<double>(result.inserted_count), "ok");
  return result;
}

}  // namespace scratchbird::engine::internal_api
