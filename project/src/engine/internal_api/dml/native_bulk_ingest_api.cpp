// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/native_bulk_ingest_api.hpp"

#include "api_diagnostics.hpp"
#include "dml/insert_physical_integration.hpp"
#include "observability/dml_summary_counters.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kOperationId = "dml.execute_native_bulk_ingest";
constexpr const char* kDisabledDiagnostic = "DML.NATIVE_BULK_INGEST.DISABLED";

void AddNativeBulkIngestEvidence(EngineExecuteNativeBulkIngestResult* result,
                                 bool enabled) {
  result->evidence.push_back({"native_bulk_ingest", enabled ? "enabled" : "disabled"});
  result->evidence.push_back({"native_bulk_ingest_enabled", enabled ? "true" : "false"});
  result->evidence.push_back({"native_bulk_ingest_route", "engine_internal_api"});
  result->evidence.push_back({"native_bulk_ingest_source", "binary_typed_rows"});
  result->evidence.push_back({"native_bulk_ingest_batch_format",
                              "engine_binary_row_batch"});
  result->evidence.push_back({"parser_finality_authority", "false"});
  result->evidence.push_back({"reference_finality_authority", "false"});
  result->evidence.push_back({"native_bulk_ingest_cluster_private", "false"});
}

EngineExecuteNativeBulkIngestResult NativeFailure(
    const EngineExecuteNativeBulkIngestRequest& request,
    EngineApiDiagnostic diagnostic,
    bool enabled) {
  EngineExecuteNativeBulkIngestResult result;
  result.ok = false;
  result.operation_id = kOperationId;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.primary_object = request.target_table;
  result.diagnostics.push_back(std::move(diagnostic));
  AddNativeBulkIngestEvidence(&result, enabled);
  return result;
}

bool NativeBulkIngestEnabled(const EngineExecuteNativeBulkIngestRequest& request) {
  return request.native_bulk_ingest_enabled &&
         SecurityOptionBool(request, "native_bulk_ingest_enabled:", true) &&
         SecurityOptionBool(request, "native_bulk_ingest:", true);
}

bool NativeDirectPhysicalLaneEnabled(const EngineExecuteNativeBulkIngestRequest& request) {
  return SecurityOptionBool(request, "direct_physical_lane:", true) &&
         SecurityOptionBool(request, "native_bulk_direct_physical_lane:", true);
}

EngineApiU64 EstimateRowsetBytes(std::span<const EngineRowValue> rows) {
  EngineApiU64 bytes = 0;
  for (const auto& row : rows) {
    bytes += 32;
    for (const auto& field : row.fields) {
      bytes += static_cast<EngineApiU64>(field.first.size() + 16);
      bytes += static_cast<EngineApiU64>(field.second.encoded_value.size());
    }
  }
  return bytes;
}

bool OptionKeyPresent(const std::vector<std::string>& options,
                      std::string_view key) {
  for (const auto& option : options) {
    if (option.rfind(std::string(key) + "=", 0) == 0 ||
        option.rfind(std::string(key) + ":", 0) == 0) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> SharedFieldOrderFromRows(
    std::span<const EngineRowValue> rows) {
  std::vector<std::string> order;
  if (rows.empty() || rows.front().fields.empty()) {
    return order;
  }
  order.reserve(rows.front().fields.size());
  for (const auto& field : rows.front().fields) {
    if (field.first.empty()) {
      order.clear();
      return order;
    }
    order.push_back(field.first);
  }
  for (std::size_t row_index = 1; row_index < rows.size(); ++row_index) {
    const auto& row = rows[row_index];
    if (row.fields.size() != order.size()) {
      order.clear();
      return order;
    }
    for (std::size_t field_index = 0; field_index < order.size(); ++field_index) {
      if (row.fields[field_index].first != order[field_index]) {
        order.clear();
        return order;
      }
    }
  }
  return order;
}

std::size_t AdaptiveWindowRows(const EngineExecuteNativeBulkIngestRequest& request) {
  if (request.canonical_rows.size() <= 1) {
    return request.canonical_rows.size();
  }
  constexpr EngineApiU64 kDefaultTargetBytes = 4u * 1024u * 1024u;
  constexpr std::size_t kMinRows = 1024;
  constexpr std::size_t kMaxRows = 50000;
  const std::size_t sample_rows = std::min<std::size_t>(request.canonical_rows.size(), 256);
  const EngineApiU64 sample_bytes = EstimateRowsetBytes(
      std::span<const EngineRowValue>(request.canonical_rows.data(), sample_rows));
  const EngineApiU64 average_bytes =
      std::max<EngineApiU64>(1, sample_bytes / static_cast<EngineApiU64>(sample_rows));
  std::size_t rows = static_cast<std::size_t>(kDefaultTargetBytes / average_bytes);
  rows = std::clamp(rows, kMinRows, kMaxRows);
  return std::min(rows, request.canonical_rows.size());
}

scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendRequest
MakeDirectPhysicalRequest(const EngineExecuteNativeBulkIngestRequest& request,
                          std::size_t first_row,
                          std::size_t row_count) {
  scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendRequest direct;
  direct.context = request.context;
  direct.target_table = request.target_table;
  direct.borrowed_input_rows = std::span<const EngineRowValue>(
      request.canonical_rows.data() + first_row,
      row_count);
  if (!request.shared_row_field_order.empty()) {
    direct.shared_row_field_order = std::span<const std::string>(
        request.shared_row_field_order.data(),
        request.shared_row_field_order.size());
  } else {
    direct.owned_shared_row_field_order =
        SharedFieldOrderFromRows(direct.borrowed_input_rows);
    if (!direct.owned_shared_row_field_order.empty()) {
      direct.shared_row_field_order = std::span<const std::string>(
          direct.owned_shared_row_field_order.data(),
          direct.owned_shared_row_field_order.size());
    }
  }
  direct.option_envelopes = request.option_envelopes;
  if (!direct.shared_row_field_order.empty() &&
      !OptionKeyPresent(direct.option_envelopes,
                        "sblr.canonical_rowset_shared_shape")) {
    direct.option_envelopes.push_back("sblr.canonical_rowset_shared_shape=true");
  }
  if (first_row == 0 && row_count >= request.canonical_rows.size() &&
      !OptionKeyPresent(direct.option_envelopes, "native_bulk.single_window")) {
    direct.option_envelopes.push_back("native_bulk.single_window=true");
  }
  direct.diagnostic_options = request.diagnostic_options;
  direct.estimated_row_count = static_cast<EngineApiU64>(row_count);
  direct.lane_operation = "native_bulk";
  direct.duplicate_mode = request.duplicate_mode;
  direct.require_generated_row_uuid = request.require_generated_row_uuid;
  direct.strict_bulk_load_requested = request.import_policy.strict_bulk_load_requested;
  direct.direct_lane_enabled = NativeDirectPhysicalLaneEnabled(request);
  return direct;
}

scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendRequest
MakeDirectPhysicalRequest(const EngineExecuteNativeBulkIngestRequest& request) {
  return MakeDirectPhysicalRequest(request, 0, request.canonical_rows.size());
}

EngineExecuteNativeBulkIngestResult WrapDirectPhysicalResult(
    const EngineExecuteNativeBulkIngestRequest& request,
    scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendResult direct) {
  EngineExecuteNativeBulkIngestResult result;
  result.ok = direct.ok;
  result.operation_id = kOperationId;
  result.diagnostics = std::move(direct.diagnostics);
  result.unsupported_features = std::move(direct.unsupported_features);
  result.evidence = std::move(direct.evidence);
  result.result_shape = std::move(direct.result_shape);
  result.primary_object = direct.primary_object;
  result.catalog_row_uuid = direct.catalog_row_uuid;
  result.transaction_uuid = direct.transaction_uuid;
  result.local_transaction_id = direct.local_transaction_id;
  result.embedded_trust_mode_observed = direct.embedded_trust_mode_observed;
  result.cluster_authority_required = false;
  result.accepted_rows = direct.accepted_rows;
  result.inserted_rows = direct.inserted_rows;
  result.rejected_rows = direct.rejected_rows;
  result.row_uuids = std::move(direct.row_uuids);
  result.delegated_to_import_execution = false;
  result.dml_summary = std::move(direct.dml_summary);
  AddNativeBulkIngestEvidence(&result, true);
  result.evidence.push_back({"native_bulk_ingest_lane", "direct_physical"});
  result.evidence.push_back({"native_bulk_ingest_delegate", "none"});
  result.evidence.push_back({"native_bulk_ingest_import_source_kind", "none"});
  result.evidence.push_back({"native_bulk_ingest_import_format_family", "none"});
  result.evidence.push_back({"orh_210_native_direct_bulk_ingest",
                             result.ok ? "runtime_consumed" : "refused"});
  if (!result.ok) {
    result.evidence.push_back({"native_bulk_ingest_refused_by",
                               "dml.direct_physical_bulk_append"});
  }
  if (result.primary_object.uuid.canonical.empty()) {
    result.primary_object = request.target_table;
  }
  return result;
}

}  // namespace

EngineExecuteNativeBulkIngestResult EngineExecuteNativeBulkIngest(
    const EngineExecuteNativeBulkIngestRequest& request) {
  const bool enabled = NativeBulkIngestEnabled(request);
  if (!enabled) {
    return NativeFailure(
        request,
        MakeEngineApiDiagnostic(kDisabledDiagnostic,
                                "dml.native_bulk_ingest.disabled",
                                "native_bulk_ingest_enabled:false",
                                true),
        false);
  }
  if (request.context.local_transaction_id == 0) {
    return NativeFailure(
        request,
        MakeInvalidRequestDiagnostic(kOperationId, "local_transaction_id_required"),
        true);
  }
  if (!request.localized_names.empty()) {
    return NativeFailure(
        request,
        MakeInvalidRequestDiagnostic(kOperationId, "localized_names_not_allowed_engine_boundary"),
        true);
  }
  if (request.target_table.uuid.canonical.empty()) {
    return NativeFailure(
        request,
        MakeInvalidRequestDiagnostic(kOperationId, "target_table_uuid_required"),
        true);
  }
  if (request.canonical_rows.empty()) {
    return NativeFailure(
        request,
        MakeInvalidRequestDiagnostic(kOperationId, "canonical_rows_required"),
        true);
  }

  const std::size_t window_rows = AdaptiveWindowRows(request);
  if (window_rows == 0 || window_rows >= request.canonical_rows.size()) {
    auto result = WrapDirectPhysicalResult(
        request,
        scratchbird::engine::internal_api::dml::ExecuteDirectPhysicalBulkAppend(
            MakeDirectPhysicalRequest(request)));
    result.evidence.push_back({"native_bulk_adaptive_windowing", "single_window"});
    result.evidence.push_back({"native_bulk_adaptive_window_rows",
                               std::to_string(request.canonical_rows.size())});
    result.evidence.push_back({"native_bulk_adaptive_window_count", "1"});
    return result;
  }

  EngineExecuteNativeBulkIngestResult result;
  result.ok = true;
  result.operation_id = kOperationId;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.primary_object = request.target_table;
  result.delegated_to_import_execution = false;
  AddNativeBulkIngestEvidence(&result, true);
  result.evidence.push_back({"native_bulk_ingest_lane", "direct_physical"});
  result.evidence.push_back({"native_bulk_ingest_delegate", "none"});
  result.evidence.push_back({"native_bulk_ingest_import_source_kind", "none"});
  result.evidence.push_back({"native_bulk_ingest_import_format_family", "none"});
  result.evidence.push_back({"native_bulk_adaptive_windowing", "enabled"});
  result.evidence.push_back({"native_bulk_adaptive_window_rows",
                             std::to_string(window_rows)});
  result.evidence.push_back({"native_bulk_adaptive_window_count",
                             std::to_string((request.canonical_rows.size() + window_rows - 1) /
                                            window_rows)});

  for (std::size_t first = 0; first < request.canonical_rows.size(); first += window_rows) {
    const std::size_t count =
        std::min(window_rows, request.canonical_rows.size() - first);
    auto direct = scratchbird::engine::internal_api::dml::ExecuteDirectPhysicalBulkAppend(
        MakeDirectPhysicalRequest(request, first, count));
    if (!direct.ok) {
      auto failure = WrapDirectPhysicalResult(request, std::move(direct));
      failure.evidence.push_back({"native_bulk_adaptive_windowing", "failed_window"});
      failure.evidence.push_back({"native_bulk_adaptive_window_first_row",
                                  std::to_string(first)});
      failure.evidence.push_back({"native_bulk_adaptive_window_row_count",
                                  std::to_string(count)});
      return failure;
    }
    result.accepted_rows += direct.accepted_rows;
    result.inserted_rows += direct.inserted_rows;
    result.rejected_rows += direct.rejected_rows;
    AddDmlSummaryCounters(&result.dml_summary, direct.dml_summary);
    if (result.result_shape.result_kind.empty()) {
      result.result_shape = direct.result_shape;
    }
    if (result.catalog_row_uuid.canonical.empty()) {
      result.catalog_row_uuid = direct.catalog_row_uuid;
    }
    if (result.primary_object.uuid.canonical.empty()) {
      result.primary_object = direct.primary_object;
    }
    if (request.canonical_rows.size() <= 10000) {
      result.row_uuids.insert(result.row_uuids.end(),
                              direct.row_uuids.begin(),
                              direct.row_uuids.end());
    }
    result.evidence.insert(result.evidence.end(),
                           std::make_move_iterator(direct.evidence.begin()),
                           std::make_move_iterator(direct.evidence.end()));
  }
  result.dml_summary.rows_changed = result.inserted_rows;
  AddDmlSummaryEvidence(&result);
  result.evidence.push_back({"orh_210_native_direct_bulk_ingest",
                             "runtime_consumed"});
  return result;
}

}  // namespace scratchbird::engine::internal_api
