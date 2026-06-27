// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/native_bulk_ingest_api.hpp"

#include "api_diagnostics.hpp"
#include "dml/dml_executable_trigger_runtime.hpp"
#include "dml/insert_api.hpp"
#include "dml/insert_physical_integration.hpp"
#include "observability/dml_summary_counters.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <limits>
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

std::size_t NativeBulkRequestRowCount(
    const EngineExecuteNativeBulkIngestRequest& request) {
  if (!request.canonical_rows.empty()) {
    return request.canonical_rows.size();
  }
  if (!request.native_row_packet.present || request.native_row_packet.row_count == 0) {
    return 0;
  }
  if (request.native_row_packet.row_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return 0;
  }
  return static_cast<std::size_t>(request.native_row_packet.row_count);
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
  const std::size_t row_count = NativeBulkRequestRowCount(request);
  if (row_count <= 1) {
    return row_count;
  }
  if (request.canonical_rows.empty() && request.native_row_packet.present) {
    return row_count;
  }
  // Native row packets are already materialized before this stage.  Keep
  // fixed-shape COPY frames in large physical append windows so the direct
  // lane does not pay descriptor and index-cache setup repeatedly.
  constexpr EngineApiU64 kDefaultTargetBytes = 32u * 1024u * 1024u;
  constexpr std::size_t kMinRows = 1024;
  constexpr std::size_t kMaxRows = 250000;
  const std::size_t sample_rows = std::min<std::size_t>(request.canonical_rows.size(), 256);
  const EngineApiU64 sample_bytes = EstimateRowsetBytes(
      std::span<const EngineRowValue>(request.canonical_rows.data(), sample_rows));
  const EngineApiU64 average_bytes =
      std::max<EngineApiU64>(1, sample_bytes / static_cast<EngineApiU64>(sample_rows));
  std::size_t rows = static_cast<std::size_t>(kDefaultTargetBytes / average_bytes);
  rows = std::clamp(rows, kMinRows, kMaxRows);
  return std::min(rows, row_count);
}

scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendRequest
MakeDirectPhysicalRequest(const EngineExecuteNativeBulkIngestRequest& request,
                          std::size_t first_row,
                          std::size_t row_count) {
  scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendRequest direct;
  direct.context = request.context;
  direct.target_table = request.target_table;
  if (!request.canonical_rows.empty()) {
    direct.borrowed_input_rows = std::span<const EngineRowValue>(
        request.canonical_rows.data() + first_row,
        row_count);
  }
  if (request.native_row_packet.present &&
      first_row == 0 &&
      row_count == NativeBulkRequestRowCount(request) &&
      request.native_row_packet.row_count == NativeBulkRequestRowCount(request)) {
    direct.native_row_packet = &request.native_row_packet;
  }
  if (!request.shared_row_field_order.empty()) {
    direct.shared_row_field_order = std::span<const std::string>(
        request.shared_row_field_order.data(),
        request.shared_row_field_order.size());
  } else if (request.native_row_packet.present &&
             !request.native_row_packet.field_order.empty()) {
    direct.shared_row_field_order = std::span<const std::string>(
        request.native_row_packet.field_order.data(),
        request.native_row_packet.field_order.size());
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
  return MakeDirectPhysicalRequest(request, 0, NativeBulkRequestRowCount(request));
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

EngineInsertRowsRequest MakeTriggerAwareInsertRequest(
    const EngineExecuteNativeBulkIngestRequest& request,
    std::span<const EngineRowValue> rows) {
  EngineInsertRowsRequest insert;
  insert.context = request.context;
  insert.operation_id = "dml.insert_rows";
  insert.target_table = request.target_table;
  insert.borrowed_input_rows = rows;
  insert.require_generated_row_uuid = request.require_generated_row_uuid;
  insert.estimated_row_count = static_cast<EngineApiU64>(rows.size());
  insert.insert_mode = "native_bulk_trigger_aware";
  insert.duplicate_mode = request.duplicate_mode;
  insert.strict_bulk_load_requested =
      request.import_policy.strict_bulk_load_requested;
  insert.option_envelopes = request.option_envelopes;
  insert.diagnostic_options = request.diagnostic_options;
  return insert;
}

EngineExecuteNativeBulkIngestResult WrapTriggerAwareInsertResult(
    const EngineExecuteNativeBulkIngestRequest& request,
    EngineInsertRowsResult insert,
    std::size_t input_row_count) {
  EngineExecuteNativeBulkIngestResult result;
  result.ok = insert.ok;
  result.operation_id = kOperationId;
  result.diagnostics = std::move(insert.diagnostics);
  result.unsupported_features = std::move(insert.unsupported_features);
  result.evidence = std::move(insert.evidence);
  result.result_shape = std::move(insert.result_shape);
  result.primary_object = insert.primary_object;
  result.catalog_row_uuid = insert.catalog_row_uuid;
  result.transaction_uuid = insert.transaction_uuid;
  result.local_transaction_id = insert.local_transaction_id;
  result.embedded_trust_mode_observed = insert.embedded_trust_mode_observed;
  result.cluster_authority_required = false;
  result.accepted_rows = insert.ok
                             ? insert.inserted_count + insert.updated_count +
                                   insert.skipped_count
                             : 0;
  result.inserted_rows = insert.inserted_count;
  result.rejected_rows =
      insert.ok ? 0 : static_cast<EngineApiU64>(input_row_count);
  result.row_uuids = std::move(insert.row_uuids);
  result.delegated_to_import_execution = false;
  result.dml_summary = std::move(insert.dml_summary);
  AddNativeBulkIngestEvidence(&result, true);
  result.evidence.push_back({"native_bulk_ingest_lane", "trigger_aware_insert"});
  result.evidence.push_back({"native_bulk_ingest_delegate", "dml.insert_rows"});
  result.evidence.push_back({"native_bulk_ingest_import_source_kind", "none"});
  result.evidence.push_back({"native_bulk_ingest_import_format_family", "none"});
  result.evidence.push_back({"native_bulk_ingest_trigger_aware_fallback",
                             result.ok ? "executed" : "refused"});
  result.evidence.push_back({"orh_210_native_direct_bulk_ingest",
                             result.ok ? "trigger_aware_insert_path"
                                       : "trigger_aware_insert_refused"});
  if (!result.ok) {
    result.evidence.push_back({"native_bulk_ingest_refused_by",
                               "dml.insert_rows"});
  }
  if (result.primary_object.uuid.canonical.empty()) {
    result.primary_object = request.target_table;
  }
  if (result.transaction_uuid.canonical.empty()) {
    result.transaction_uuid = request.context.transaction_uuid;
  }
  if (result.local_transaction_id == 0) {
    result.local_transaction_id = request.context.local_transaction_id;
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
  const std::size_t row_count = NativeBulkRequestRowCount(request);
  if (row_count == 0) {
    return NativeFailure(
        request,
        MakeInvalidRequestDiagnostic(kOperationId, "native_rowset_required"),
        true);
  }
  if (dml_trigger_runtime::HasActiveTableTriggerDescriptors(
          request.context,
          request.target_table.uuid.canonical)) {
    if (request.canonical_rows.empty()) {
      return NativeFailure(
          request,
          MakeEngineApiDiagnostic(
              "DML.NATIVE_BULK_INGEST.TRIGGER_AWARE_CANONICAL_ROWS_REQUIRED",
              "dml.native_bulk_ingest.trigger_aware_canonical_rows_required",
              "native row packet trigger fallback requires canonical rows so "
              "the trigger-aware insert path can validate and fire executable "
              "trigger descriptors",
              true),
          true);
    }
    const auto rows = std::span<const EngineRowValue>(
        request.canonical_rows.data(),
        request.canonical_rows.size());
    return WrapTriggerAwareInsertResult(
        request,
        EngineInsertRows(MakeTriggerAwareInsertRequest(request, rows)),
        rows.size());
  }
  if (request.canonical_rows.empty() && request.native_row_packet.present &&
      request.native_row_packet.row_count != row_count) {
    return NativeFailure(
        request,
        MakeInvalidRequestDiagnostic(kOperationId, "native_row_packet_row_count_mismatch"),
        true);
  }

  const std::size_t window_rows = AdaptiveWindowRows(request);
  if (window_rows == 0 || window_rows >= row_count) {
    auto result = WrapDirectPhysicalResult(
        request,
        scratchbird::engine::internal_api::dml::ExecuteDirectPhysicalBulkAppend(
            MakeDirectPhysicalRequest(request)));
    result.evidence.push_back({"native_bulk_adaptive_windowing", "single_window"});
    result.evidence.push_back({"native_bulk_adaptive_window_rows",
                               std::to_string(row_count)});
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
                             std::to_string((row_count + window_rows - 1) /
                                            window_rows)});

  for (std::size_t first = 0; first < row_count; first += window_rows) {
    const std::size_t count =
        std::min(window_rows, row_count - first);
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
    if (row_count <= 10000) {
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
