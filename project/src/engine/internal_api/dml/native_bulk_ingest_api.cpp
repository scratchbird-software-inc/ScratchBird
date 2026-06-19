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
#include "security/security_model.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kOperationId = "dml.execute_native_bulk_ingest";
constexpr const char* kDisabledDiagnostic = "DML.NATIVE_BULK_INGEST.DISABLED";

using NativeBulkPhaseClock = std::chrono::steady_clock;

struct NativeBulkPhaseTiming {
  std::string phase;
  std::uint64_t elapsed_us = 0;
};

std::uint64_t NativeBulkElapsedMicros(NativeBulkPhaseClock::time_point start,
                                      NativeBulkPhaseClock::time_point finish) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count());
}

std::string NativeBulkTraceJsonEscape(std::string_view value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4u) & 0x0fu] << kHex[ch & 0x0fu];
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

void WriteNativeBulkPhaseTraceIfRequested(
    const EngineExecuteNativeBulkIngestRequest& request,
    bool ok,
    std::uint64_t rows,
    const std::vector<NativeBulkPhaseTiming>& phases) {
  const char* trace_path = std::getenv("SCRATCHBIRD_NATIVE_BULK_API_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  static std::mutex trace_mutex;
  std::lock_guard<std::mutex> guard(trace_mutex);
  std::ofstream out(trace_path, std::ios::app);
  if (!out) {
    return;
  }
  std::uint64_t total_us = 0;
  for (const auto& phase : phases) {
    total_us += phase.elapsed_us;
  }
  out << "{\"operation_id\":\"" << kOperationId << "\""
      << ",\"table_uuid\":\""
      << NativeBulkTraceJsonEscape(request.target_table.uuid.canonical) << "\""
      << ",\"rows\":" << rows
      << ",\"ok\":" << (ok ? "true" : "false")
      << ",\"total_us\":" << total_us
      << ",\"phases\":{";
  bool first = true;
  for (const auto& phase : phases) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << '"' << NativeBulkTraceJsonEscape(phase.phase)
        << "\":" << phase.elapsed_us;
  }
  out << "}}\n";
}

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

scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendRequest
MakeDirectPhysicalRequest(const EngineExecuteNativeBulkIngestRequest& request) {
  scratchbird::engine::internal_api::dml::DirectPhysicalBulkAppendRequest direct;
  direct.context = request.context;
  direct.target_table = request.target_table;
  direct.borrowed_input_rows = std::span<const EngineRowValue>(
      request.canonical_rows.data(),
      request.canonical_rows.size());
  direct.option_envelopes = request.option_envelopes;
  direct.diagnostic_options = request.diagnostic_options;
  direct.estimated_row_count = request.estimated_row_count == 0
                                   ? static_cast<EngineApiU64>(request.canonical_rows.size())
                                   : request.estimated_row_count;
  direct.lane_operation = "native_bulk";
  direct.duplicate_mode = request.duplicate_mode;
  direct.require_generated_row_uuid = request.require_generated_row_uuid;
  direct.strict_bulk_load_requested = request.import_policy.strict_bulk_load_requested;
  direct.direct_lane_enabled = NativeDirectPhysicalLaneEnabled(request);
  return direct;
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
  std::vector<NativeBulkPhaseTiming> phase_timings;
  auto phase_start = NativeBulkPhaseClock::now();
  auto record_phase = [&](std::string phase,
                          NativeBulkPhaseClock::time_point start) {
    if (std::getenv("SCRATCHBIRD_NATIVE_BULK_API_PHASE_TRACE_FILE") == nullptr) {
      return;
    }
    phase_timings.push_back(
        {std::move(phase),
         NativeBulkElapsedMicros(start, NativeBulkPhaseClock::now())});
  };
  const bool enabled = NativeBulkIngestEnabled(request);
  record_phase("policy_enabled_check", phase_start);
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

  phase_start = NativeBulkPhaseClock::now();
  auto direct_request = MakeDirectPhysicalRequest(request);
  record_phase("make_direct_request", phase_start);
  phase_start = NativeBulkPhaseClock::now();
  auto direct_result =
      scratchbird::engine::internal_api::dml::ExecuteDirectPhysicalBulkAppend(
          direct_request);
  record_phase("execute_direct_physical", phase_start);
  phase_start = NativeBulkPhaseClock::now();
  auto result = WrapDirectPhysicalResult(request, std::move(direct_result));
  record_phase("wrap_direct_result", phase_start);
  WriteNativeBulkPhaseTraceIfRequested(request,
                                       result.ok,
                                       result.accepted_rows,
                                       phase_timings);
  return result;
}

}  // namespace scratchbird::engine::internal_api
