// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/select_api.hpp"

#include "crud_support/crud_store.hpp"
#include "dml/serializable_mutation_guard.hpp"
#include "domain_support/domain_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
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
  std::stable_sort(rows->begin(), rows->end(), [&](const CrudRowVersionRecord& lhs, const CrudRowVersionRecord& rhs) {
    const std::string left_value = CrudFieldValue(lhs.values, column);
    const std::string right_value = CrudFieldValue(rhs.values, column);
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
  return predicate.predicate_kind == "column_equals" ||
         predicate.predicate_kind == "column_in_list" ||
         predicate.predicate_kind == "column_range" ||
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
  const std::string table_uuid = !request.source_object.uuid.canonical.empty() ? request.source_object.uuid.canonical : request.target_object.uuid.canonical;
  if (table_uuid.empty()) {
    return MakeCrudDiagnosticResult<EngineSelectRowsResult>(request.context, "dml.select_rows", MakeInvalidRequestDiagnostic("dml.select_rows", "source_table_uuid_required"));
  }
  const auto loaded = LoadMgaRelationStoreState(request.context);
  if (!loaded.ok) { return MakeCrudDiagnosticResult<EngineSelectRowsResult>(request.context, "dml.select_rows", loaded.diagnostic); }
  CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto table = FindVisibleCrudTable(state, table_uuid, request.context.local_transaction_id);
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
  const EnginePredicateEnvelope& predicate =
      !request.select_predicate.predicate_kind.empty() ? request.select_predicate : request.predicate;
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
  std::string index_uuid_used;
  std::string row_scan_predicate;
  std::vector<EngineEvidenceReference> index_lookup_evidence;
  if (predicate.predicate_kind == "row_uuid_match" && !predicate.canonical_predicate_envelope.empty()) {
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
    }
  }
  if (!rows_ready) {
    rows = VisibleCrudRowsForContext(state, table_uuid, request.context);
    rows_ready = true;
  }
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
  ApplyOrdering(ordering, &rows);
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
  ApplyProjection(projection, &rows);
  auto result = MakeCrudSuccessResult<EngineSelectRowsResult>(request.context, "dml.select_rows");
  result.visible_count = rows.size();
  result.result_shape = CrudRowsToResultShape(rows);
  if (!index_uuid_used.empty()) { result.evidence.push_back({"index_lookup", index_uuid_used}); }
  if (!row_scan_predicate.empty()) { result.evidence.push_back({"row_scan_predicate", row_scan_predicate}); }
  result.evidence.insert(result.evidence.end(),
                         index_lookup_evidence.begin(),
                         index_lookup_evidence.end());
  auto serializable_recorded = dml::RecordSerializableSelectRead(
      request.context,
      "dml.select_rows",
      table_uuid,
      predicate,
      request.option_envelopes);
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
  return result;
}

}  // namespace scratchbird::engine::internal_api
