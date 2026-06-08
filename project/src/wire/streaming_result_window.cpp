// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "streaming_result_window.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::wire {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

void AddBool(std::vector<std::string>* evidence,
             const std::string& key,
             bool value) {
  Add(evidence, key + "=" + (value ? "true" : "false"));
}

std::uint64_t SafeMul(std::uint64_t left, std::uint64_t right) {
  if (left == 0 || right == 0) {
    return 0;
  }
  constexpr std::uint64_t kMax = ~std::uint64_t{0};
  if (left > kMax / right) {
    return kMax;
  }
  return left * right;
}

std::uint64_t PositiveOrOne(std::uint64_t value) {
  return value == 0 ? 1 : value;
}

StreamingResultWindowDecision BaseDecision(
    const StreamingResultWindowRequest& request) {
  StreamingResultWindowDecision decision;
  decision.surface = request.surface;
  decision.frame_start_row = request.fetch_offset;
  decision.frame_sequence = request.fetch_offset /
                            PositiveOrOne(request.max_window_rows);
  Add(&decision.evidence, kStreamingResultWindowSearchKey);
  Add(&decision.evidence,
      std::string("streaming_surface=") +
          StreamingResultSurfaceName(request.surface));
  AddBool(&decision.evidence,
          "wire_streaming_frames_enabled",
          request.streaming_frames_enabled);
  Add(&decision.evidence,
      "cursor_fetch_offset=" + std::to_string(request.fetch_offset));
  Add(&decision.evidence,
      "cursor_requested_fetch_rows=" +
          std::to_string(request.requested_fetch_rows));
  Add(&decision.evidence,
      "cursor_max_window_rows=" + std::to_string(request.max_window_rows));
  Add(&decision.evidence,
      "binary_frame_max_bytes=" + std::to_string(request.max_frame_bytes));
  Add(&decision.evidence,
      "streaming_memory_budget_bytes=" +
          std::to_string(request.memory_budget_bytes));
  Add(&decision.evidence,
      "result_total_rows=" + std::to_string(request.total_rows));
  Add(&decision.evidence,
      "result_row_width_bytes=" + std::to_string(request.row_width_bytes));
  AddBool(&decision.evidence,
          "continuation_token_present",
          request.continuation_token_present);
  AddBool(&decision.evidence,
          "server_side_cancellation_requested",
          request.server_cancel_requested);
  AddBool(&decision.evidence,
          "client_backpressure_present",
          request.client_backpressure);
  AddBool(&decision.evidence,
          "ordering_key_present",
          request.ordering_key_present);
  AddBool(&decision.evidence,
          "base_row_mga_recheck_required",
          request.base_row_mga_recheck_required);
  AddBool(&decision.evidence,
          "base_row_security_recheck_required",
          request.base_row_security_recheck_required);
  AddBool(&decision.evidence,
          "parser_or_donor_authority",
          request.parser_or_donor_authority);
  AddBool(&decision.evidence,
          "client_visibility_or_finality_authority",
          request.client_visibility_or_finality_authority);
  AddBool(&decision.evidence,
          "write_ahead_or_finality_authority",
          request.write_ahead_or_finality_authority);
  Add(&decision.evidence, "cursor_metadata_authority=advisory_only");
  Add(&decision.evidence, "frame_metadata_authority=advisory_only");
  AddBool(&decision.evidence, "benchmark_clean", request.benchmark_clean);
  Add(&decision.evidence, "support_bundle_ready=true");
  return decision;
}

StreamingResultWindowDecision Finish(StreamingResultWindowDecision decision,
                                     StreamingResultWindowAction action,
                                     std::string diagnostic_code,
                                     std::string diagnostic_detail,
                                     bool accepted,
                                     bool fail_closed) {
  decision.action = action;
  decision.diagnostic_code = std::move(diagnostic_code);
  decision.diagnostic_detail = std::move(diagnostic_detail);
  decision.accepted = accepted;
  decision.fail_closed = fail_closed;
  Add(&decision.evidence,
      "streaming_window_action=" +
          std::string(StreamingResultWindowActionName(action)));
  Add(&decision.evidence,
      "streaming_diagnostic=" + decision.diagnostic_code);
  if (!decision.diagnostic_detail.empty()) {
    Add(&decision.evidence,
        "streaming_diagnostic_detail=" + decision.diagnostic_detail);
  }
  Add(&decision.evidence,
      "streaming_window_rows=" + std::to_string(decision.window_rows));
  Add(&decision.evidence,
      "binary_frame_sequence=" + std::to_string(decision.frame_sequence));
  Add(&decision.evidence,
      "binary_frame_start_row=" + std::to_string(decision.frame_start_row));
  Add(&decision.evidence,
      "binary_frame_next_fetch_offset=" +
          std::to_string(decision.next_fetch_offset));
  Add(&decision.evidence,
      "binary_frame_estimated_bytes=" +
          std::to_string(decision.estimated_frame_bytes));
  AddBool(&decision.evidence,
          "binary_frame_continuation_required",
          decision.continuation_required);
  AddBool(&decision.evidence,
          "binary_frame_continuation_token_issued",
          decision.continuation_required);
  AddBool(&decision.evidence,
          "result_ordering_preserved",
          decision.ordering_preserved);
  Add(&decision.evidence, "result_full_materialization_required=false");
  AddBool(&decision.evidence,
          "exact_refusal",
          decision.exact_refusal);
  Add(&decision.evidence, "server_side_cancellation_supported=true");
  Add(&decision.evidence,
      "client_backpressure_behavior=" +
          std::string(action == StreamingResultWindowAction::kBackpressureWait
                          ? "pause_cursor_until_client_credit"
                          : "credit_checked_per_window"));
  return decision;
}

StreamingResultWindowDecision Refuse(StreamingResultWindowDecision decision,
                                     std::string diagnostic_code,
                                     std::string diagnostic_detail) {
  decision.exact_refusal = true;
  return Finish(std::move(decision),
                StreamingResultWindowAction::kRefuse,
                std::move(diagnostic_code),
                std::move(diagnostic_detail),
                false,
                true);
}

std::uint64_t RowsForByteLimit(std::uint64_t byte_limit,
                               std::uint64_t row_width_bytes) {
  if (row_width_bytes == 0) {
    return 0;
  }
  return byte_limit / row_width_bytes;
}

}  // namespace

const char* StreamingResultSurfaceName(StreamingResultSurface surface) {
  switch (surface) {
    case StreamingResultSurface::kSql:
      return "sql";
    case StreamingResultSurface::kDocument:
      return "document";
    case StreamingResultSurface::kSearch:
      return "search";
    case StreamingResultSurface::kVector:
      return "vector";
    case StreamingResultSurface::kGraph:
      return "graph";
    case StreamingResultSurface::kTimeSeries:
      return "time_series";
  }
  return "sql";
}

const char* StreamingResultWindowActionName(
    StreamingResultWindowAction action) {
  switch (action) {
    case StreamingResultWindowAction::kStreamingFrameWindow:
      return "streaming_frame_window";
    case StreamingResultWindowAction::kBackpressureWait:
      return "backpressure_wait";
    case StreamingResultWindowAction::kCancelled:
      return "cancelled";
    case StreamingResultWindowAction::kBoundedLegacyFetch:
      return "bounded_legacy_fetch";
    case StreamingResultWindowAction::kRefuse:
      return "refuse";
  }
  return "refuse";
}

StreamingResultWindowRequest DefaultStreamingResultWindowRequest(
    StreamingResultSurface surface) {
  StreamingResultWindowRequest request;
  request.surface = surface;
  request.streaming_frames_enabled = true;
  request.total_rows = 10000;
  request.row_width_bytes = 128;
  request.requested_fetch_rows = 512;
  request.max_window_rows = 512;
  request.max_frame_bytes = 128 * 512;
  request.memory_budget_bytes = 128 * 512;
  request.fetch_offset = 0;
  request.continuation_token_present = true;
  request.legacy_fetch_row_bound = 512;
  return request;
}

StreamingResultWindowDecision EvaluateStreamingResultWindow(
    const StreamingResultWindowRequest& request) {
  auto decision = BaseDecision(request);
  if (request.parser_or_donor_authority) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.UNSAFE_PARSER_OR_DONOR_AUTHORITY",
                  "parser_or_donor_authority_forbidden");
  }
  if (request.client_visibility_or_finality_authority) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.UNSAFE_CLIENT_AUTHORITY",
                  "client_visibility_or_finality_authority_forbidden");
  }
  if (request.write_ahead_or_finality_authority) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.UNSAFE_FINALITY_AUTHORITY",
                  "write_ahead_or_finality_authority_forbidden");
  }
  if (!request.base_row_mga_recheck_required ||
      !request.base_row_security_recheck_required) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.BASE_ROW_RECHECK_REQUIRED",
                  "base_row_mga_and_security_rechecks_required");
  }
  if (!request.ordering_key_present) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.ORDERING_PROOF_REQUIRED",
                  "ordering_key_required_for_window_continuation");
  }
  if (request.row_width_bytes == 0 || request.total_rows == 0 ||
      request.requested_fetch_rows == 0 || request.max_window_rows == 0 ||
      request.max_frame_bytes == 0 || request.memory_budget_bytes == 0) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.INVALID_RESOURCE_POLICY",
                  "nonzero_rows_width_fetch_window_frame_and_memory_required");
  }
  if (request.fetch_offset > request.total_rows) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.INVALID_CURSOR_OFFSET",
                  "fetch_offset_exceeds_total_rows");
  }
  if (request.fetch_offset > 0 && !request.continuation_token_present) {
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.CONTINUATION_TOKEN_REQUIRED",
                  "nonzero_fetch_offset_requires_continuation_token");
  }
  if (request.row_width_bytes > request.max_frame_bytes ||
      request.row_width_bytes > request.memory_budget_bytes) {
    return Refuse(
        std::move(decision),
        "WIRE.STREAMING_RESULT_WINDOW.ROW_EXCEEDS_FRAME_OR_MEMORY_POLICY",
        "single_result_row_exceeds_frame_or_memory_budget");
  }

  if (request.server_cancel_requested) {
    decision.ordering_preserved = true;
    return Finish(std::move(decision),
                  StreamingResultWindowAction::kCancelled,
                  "WIRE.STREAMING_RESULT_WINDOW.CANCELLED",
                  "server_side_cancellation_observed_before_next_window",
                  true,
                  false);
  }

  const auto remaining_rows = request.total_rows - request.fetch_offset;
  const auto byte_limited_rows =
      std::min(RowsForByteLimit(request.max_frame_bytes,
                                request.row_width_bytes),
               RowsForByteLimit(request.memory_budget_bytes,
                                request.row_width_bytes));
  const auto bounded_rows =
      std::min({request.requested_fetch_rows,
                request.max_window_rows,
                byte_limited_rows,
                remaining_rows});

  if (!request.streaming_frames_enabled) {
    const auto legacy_rows =
        std::min(request.legacy_fetch_row_bound, remaining_rows);
    if (legacy_rows != 0 && request.legacy_fetch_row_bound != 0 &&
        SafeMul(legacy_rows, request.row_width_bytes) <=
            request.memory_budget_bytes) {
      decision.window_rows = legacy_rows;
      decision.next_fetch_offset = request.fetch_offset + legacy_rows;
      decision.estimated_frame_bytes =
          SafeMul(legacy_rows, request.row_width_bytes);
      decision.continuation_required =
          decision.next_fetch_offset < request.total_rows;
      decision.ordering_preserved = true;
      Add(&decision.evidence, "wire_streaming_frames_disabled_path=bounded_legacy_fetch");
      return Finish(std::move(decision),
                    StreamingResultWindowAction::kBoundedLegacyFetch,
                    "WIRE.STREAMING_RESULT_WINDOW.BOUNDED_LEGACY_FETCH",
                    "streaming_disabled_legacy_fetch_bounded_by_resource_policy",
                    true,
                    false);
    }
    return Refuse(std::move(decision),
                  "WIRE.STREAMING_RESULT_WINDOW.FULL_MATERIALIZATION_REFUSED",
                  "streaming_disabled_full_materialization_exceeds_resource_policy");
  }

  if (request.client_backpressure) {
    decision.window_rows = 0;
    decision.next_fetch_offset = request.fetch_offset;
    decision.estimated_frame_bytes = 0;
    decision.continuation_required = remaining_rows != 0;
    decision.ordering_preserved = true;
    return Finish(std::move(decision),
                  StreamingResultWindowAction::kBackpressureWait,
                  "WIRE.STREAMING_RESULT_WINDOW.CLIENT_BACKPRESSURE_WAIT",
                  "client_credit_unavailable_window_paused",
                  true,
                  false);
  }

  decision.window_rows = bounded_rows;
  decision.next_fetch_offset = request.fetch_offset + bounded_rows;
  decision.estimated_frame_bytes = SafeMul(bounded_rows, request.row_width_bytes);
  decision.continuation_required =
      decision.next_fetch_offset < request.total_rows;
  decision.ordering_preserved = true;
  return Finish(std::move(decision),
                StreamingResultWindowAction::kStreamingFrameWindow,
                "WIRE.STREAMING_RESULT_WINDOW.STREAMING_FRAME_WINDOW",
                "bounded_streaming_frame_window_selected",
                true,
                false);
}

std::vector<std::string> StreamingResultWindowEvidence(
    const StreamingResultWindowDecision& decision) {
  return decision.evidence;
}

std::vector<std::string> ResultBatchTransferStreamingWindowEvidenceAdapter(
    const StreamingResultWindowDecision& decision) {
  auto evidence = StreamingResultWindowEvidence(decision);
  evidence.push_back("result_batch_transfer_streaming_adapter=true");
  evidence.push_back("result_batch_transfer_full_materialization_required=false");
  evidence.push_back("result_batch_transfer_ordering_preserved=true");
  return evidence;
}

}  // namespace scratchbird::wire
