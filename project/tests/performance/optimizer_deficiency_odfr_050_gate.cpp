// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "streaming_result_window.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using scratchbird::wire::DefaultStreamingResultWindowRequest;
using scratchbird::wire::EvaluateStreamingResultWindow;
using scratchbird::wire::ResultBatchTransferStreamingWindowEvidenceAdapter;
using scratchbird::wire::StreamingResultSurface;
using scratchbird::wire::StreamingResultSurfaceName;
using scratchbird::wire::StreamingResultWindowAction;
using scratchbird::wire::StreamingResultWindowDecision;
using scratchbird::wire::StreamingResultWindowRequest;
using scratchbird::wire::kStreamingResultWindowSearchKey;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ODFR-050 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

bool HasEvidencePrefix(const std::vector<std::string>& evidence,
                       const std::string& prefix) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value.rfind(prefix, 0) == 0;
                     });
}

void RequireCommonEvidence(const StreamingResultWindowDecision& decision) {
  const auto& evidence = decision.evidence;
  Require(HasEvidence(evidence, kStreamingResultWindowSearchKey),
          "missing ODFR-050 search key");
  Require(HasEvidencePrefix(evidence, "streaming_surface="),
          "missing surface evidence");
  Require(HasEvidencePrefix(evidence, "cursor_fetch_offset="),
          "missing cursor offset evidence");
  Require(HasEvidencePrefix(evidence, "cursor_requested_fetch_rows="),
          "missing requested fetch evidence");
  Require(HasEvidencePrefix(evidence, "cursor_max_window_rows="),
          "missing fetch window evidence");
  Require(HasEvidencePrefix(evidence, "binary_frame_max_bytes="),
          "missing frame max bytes evidence");
  Require(HasEvidencePrefix(evidence, "streaming_memory_budget_bytes="),
          "missing memory budget evidence");
  Require(HasEvidencePrefix(evidence, "binary_frame_sequence="),
          "missing binary frame sequence evidence");
  Require(HasEvidencePrefix(evidence, "binary_frame_next_fetch_offset="),
          "missing continuation offset evidence");
  Require(HasEvidencePrefix(evidence, "binary_frame_estimated_bytes="),
          "missing frame bytes evidence");
  Require(HasEvidencePrefix(evidence, "binary_frame_continuation_required="),
          "missing continuation evidence");
  Require(HasEvidencePrefix(evidence,
                            "binary_frame_continuation_token_issued="),
          "missing continuation token evidence");
  Require(HasEvidence(evidence, "result_ordering_preserved=true"),
          "missing ordering preservation evidence");
  Require(HasEvidence(evidence, "result_full_materialization_required=false"),
          "result path must not require full materialization");
  Require(HasEvidence(evidence, "server_side_cancellation_supported=true"),
          "missing server cancellation support evidence");
  Require(HasEvidencePrefix(evidence, "client_backpressure_behavior="),
          "missing client backpressure behavior evidence");
  Require(HasEvidence(evidence, "cursor_metadata_authority=advisory_only"),
          "cursor metadata authority must be advisory");
  Require(HasEvidence(evidence, "frame_metadata_authority=advisory_only"),
          "frame metadata authority must be advisory");
  Require(HasEvidence(evidence, "base_row_mga_recheck_required=true"),
          "missing MGA recheck evidence");
  Require(HasEvidence(evidence, "base_row_security_recheck_required=true"),
          "missing security recheck evidence");
  Require(HasEvidence(evidence, "parser_or_reference_authority=false"),
          "parser/reference authority must be false");
  Require(HasEvidence(evidence, "client_visibility_or_finality_authority=false"),
          "client visibility/finality authority must be false");
  Require(HasEvidence(evidence, "write_ahead_or_finality_authority=false"),
          "write-ahead/finality authority must be false");
  Require(HasEvidence(evidence, "benchmark_clean=true"),
          "missing benchmark-clean evidence");
  Require(HasEvidence(evidence, "support_bundle_ready=true"),
          "missing support bundle evidence");
  Require(HasEvidencePrefix(evidence, "streaming_window_action="),
          "missing streaming action evidence");
}

void ProveStreamingSurfacesBoundedAndOrdered() {
  const std::vector<StreamingResultSurface> surfaces = {
      StreamingResultSurface::kSql,
      StreamingResultSurface::kDocument,
      StreamingResultSurface::kSearch,
      StreamingResultSurface::kVector,
      StreamingResultSurface::kGraph,
      StreamingResultSurface::kTimeSeries,
  };
  for (const auto surface : surfaces) {
    auto request = DefaultStreamingResultWindowRequest(surface);
    request.fetch_offset = 512;
    const auto decision = EvaluateStreamingResultWindow(request);
    Require(decision.accepted && !decision.fail_closed,
            std::string("streaming window not accepted for ") +
                StreamingResultSurfaceName(surface));
    Require(decision.action ==
                StreamingResultWindowAction::kStreamingFrameWindow,
            "expected streaming frame window action");
    Require(decision.window_rows <= request.max_window_rows,
            "window rows exceed max window");
    Require(decision.estimated_frame_bytes <= request.max_frame_bytes,
            "frame bytes exceed max frame");
    Require(decision.estimated_frame_bytes <= request.memory_budget_bytes,
            "frame bytes exceed memory budget");
    Require(decision.continuation_required,
            "large surface should require continuation");
    Require(HasEvidence(decision.evidence,
                        "binary_frame_continuation_token_issued=true"),
            "continuation token was not issued for next frame");
    Require(decision.ordering_preserved,
            "streaming surface ordering was not preserved");
    Require(HasEvidence(decision.evidence,
                        std::string("streaming_surface=") +
                            StreamingResultSurfaceName(surface)),
            "surface evidence mismatch");
    RequireCommonEvidence(decision);
  }
}

void ProveBinaryFrameContinuationAdapter() {
  auto request = DefaultStreamingResultWindowRequest(StreamingResultSurface::kSql);
  const auto decision = EvaluateStreamingResultWindow(request);
  Require(decision.continuation_required,
          "continuation should be required for large SQL result");
  const auto evidence =
      ResultBatchTransferStreamingWindowEvidenceAdapter(decision);
  Require(HasEvidence(evidence, "result_batch_transfer_streaming_adapter=true"),
          "result batch transfer streaming adapter evidence missing");
  Require(HasEvidence(evidence,
                      "result_batch_transfer_full_materialization_required=false"),
          "result batch transfer must not require full materialization");
  Require(HasEvidence(evidence,
                      "result_batch_transfer_ordering_preserved=true"),
          "result batch transfer ordering evidence missing");
}

void ProveMissingContinuationTokenRefuses() {
  auto request = DefaultStreamingResultWindowRequest(StreamingResultSurface::kSql);
  request.fetch_offset = 512;
  request.continuation_token_present = false;
  const auto decision = EvaluateStreamingResultWindow(request);
  Require(!decision.accepted && decision.fail_closed,
          "missing continuation token should refuse");
  Require(decision.action == StreamingResultWindowAction::kRefuse,
          "missing continuation token did not produce refuse action");
  Require(decision.diagnostic_code ==
              "WIRE.STREAMING_RESULT_WINDOW.CONTINUATION_TOKEN_REQUIRED",
          "missing continuation token diagnostic mismatch");
  Require(HasEvidence(decision.evidence, "support_bundle_ready=true"),
          "missing continuation refusal support evidence");
}

void ProveOverWideRowRefuses() {
  auto request =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kVector);
  request.row_width_bytes = request.max_frame_bytes + 1;
  const auto decision = EvaluateStreamingResultWindow(request);
  Require(!decision.accepted && decision.fail_closed,
          "over-wide row should refuse");
  Require(decision.action == StreamingResultWindowAction::kRefuse,
          "over-wide row did not produce refuse action");
  Require(decision.diagnostic_code ==
              "WIRE.STREAMING_RESULT_WINDOW.ROW_EXCEEDS_FRAME_OR_MEMORY_POLICY",
          "over-wide row diagnostic mismatch");
  Require(decision.estimated_frame_bytes == 0,
          "over-wide row produced an over-budget frame");
  Require(HasEvidence(decision.evidence, "support_bundle_ready=true"),
          "over-wide row refusal missing support evidence");
}

void ProveCancellationAndBackpressure() {
  auto cancel =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kDocument);
  cancel.server_cancel_requested = true;
  const auto cancel_decision = EvaluateStreamingResultWindow(cancel);
  Require(cancel_decision.accepted && !cancel_decision.fail_closed,
          "server cancellation should be accepted");
  Require(cancel_decision.action == StreamingResultWindowAction::kCancelled,
          "server cancellation did not produce cancelled action");
  Require(HasEvidence(cancel_decision.evidence,
                      "server_side_cancellation_requested=true"),
          "server cancellation request evidence missing");
  RequireCommonEvidence(cancel_decision);

  auto backpressure =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kSearch);
  backpressure.client_backpressure = true;
  const auto backpressure_decision =
      EvaluateStreamingResultWindow(backpressure);
  Require(backpressure_decision.accepted &&
              !backpressure_decision.fail_closed,
          "client backpressure should be accepted as wait");
  Require(backpressure_decision.action ==
              StreamingResultWindowAction::kBackpressureWait,
          "client backpressure did not pause cursor");
  Require(backpressure_decision.next_fetch_offset == backpressure.fetch_offset,
          "backpressure advanced cursor unexpectedly");
  Require(HasEvidence(backpressure_decision.evidence,
                      "client_backpressure_behavior=pause_cursor_until_client_credit"),
          "backpressure pause evidence missing");
  RequireCommonEvidence(backpressure_decision);
}

void ProveDisabledPathBoundedLegacyAndRefusal() {
  auto legacy =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kGraph);
  legacy.streaming_frames_enabled = false;
  legacy.legacy_fetch_row_bound = 64;
  legacy.memory_budget_bytes = 64 * legacy.row_width_bytes;
  const auto legacy_decision = EvaluateStreamingResultWindow(legacy);
  Require(legacy_decision.accepted && !legacy_decision.fail_closed,
          "disabled streaming bounded legacy fetch should be accepted");
  Require(legacy_decision.action ==
              StreamingResultWindowAction::kBoundedLegacyFetch,
          "disabled streaming did not use bounded legacy fetch");
  Require(legacy_decision.window_rows == legacy.legacy_fetch_row_bound,
          "bounded legacy fetch row count mismatch");
  Require(HasEvidence(legacy_decision.evidence,
                      "wire_streaming_frames_disabled_path=bounded_legacy_fetch"),
          "bounded legacy disabled-path evidence missing");
  RequireCommonEvidence(legacy_decision);

  auto refusal =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kTimeSeries);
  refusal.streaming_frames_enabled = false;
  refusal.legacy_fetch_row_bound = 0;
  const auto refusal_decision = EvaluateStreamingResultWindow(refusal);
  Require(!refusal_decision.accepted && refusal_decision.fail_closed,
          "disabled streaming unsafe materialization should refuse");
  Require(refusal_decision.action == StreamingResultWindowAction::kRefuse,
          "disabled streaming unsafe path did not refuse");
  Require(refusal_decision.exact_refusal,
          "disabled streaming refusal should be exact");
  Require(refusal_decision.diagnostic_code ==
              "WIRE.STREAMING_RESULT_WINDOW.FULL_MATERIALIZATION_REFUSED",
          "disabled streaming refusal diagnostic mismatch");
  Require(HasEvidence(refusal_decision.evidence, "support_bundle_ready=true"),
          "refusal missing support-bundle evidence");

  auto small =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kSql);
  small.streaming_frames_enabled = false;
  small.total_rows = 8;
  small.requested_fetch_rows = 8;
  small.max_window_rows = 8;
  small.legacy_fetch_row_bound = 8;
  small.memory_budget_bytes = 8 * small.row_width_bytes;
  small.max_frame_bytes = 8 * small.row_width_bytes;
  const auto small_decision = EvaluateStreamingResultWindow(small);
  Require(small_decision.accepted && !small_decision.fail_closed,
          "small safe disabled streaming result should use bounded legacy fetch");
  Require(small_decision.action ==
              StreamingResultWindowAction::kBoundedLegacyFetch,
          "small disabled streaming result did not use bounded legacy fetch");
  Require(!small_decision.continuation_required,
          "small disabled streaming result should not need continuation");
  Require(HasEvidence(small_decision.evidence,
                      "wire_streaming_frames_disabled_path=bounded_legacy_fetch"),
          "small disabled streaming legacy evidence missing");
  RequireCommonEvidence(small_decision);
}

void ProveUnsafeAuthorityRefused() {
  auto parser =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kVector);
  parser.parser_or_reference_authority = true;
  const auto parser_decision = EvaluateStreamingResultWindow(parser);
  Require(!parser_decision.accepted && parser_decision.fail_closed,
          "parser/reference authority was not refused");
  Require(parser_decision.diagnostic_code ==
              "WIRE.STREAMING_RESULT_WINDOW.UNSAFE_PARSER_OR_REFERENCE_AUTHORITY",
          "parser/reference authority diagnostic mismatch");

  auto client =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kSql);
  client.client_visibility_or_finality_authority = true;
  const auto client_decision = EvaluateStreamingResultWindow(client);
  Require(!client_decision.accepted && client_decision.fail_closed,
          "client visibility/finality authority was not refused");
  Require(client_decision.diagnostic_code ==
              "WIRE.STREAMING_RESULT_WINDOW.UNSAFE_CLIENT_AUTHORITY",
          "client authority diagnostic mismatch");

  auto finality =
      DefaultStreamingResultWindowRequest(StreamingResultSurface::kDocument);
  finality.write_ahead_or_finality_authority = true;
  const auto finality_decision = EvaluateStreamingResultWindow(finality);
  Require(!finality_decision.accepted && finality_decision.fail_closed,
          "write-ahead/finality authority was not refused");
  Require(finality_decision.diagnostic_code ==
              "WIRE.STREAMING_RESULT_WINDOW.UNSAFE_FINALITY_AUTHORITY",
          "write-ahead/finality diagnostic mismatch");
}

}  // namespace

int main() {
  ProveStreamingSurfacesBoundedAndOrdered();
  ProveBinaryFrameContinuationAdapter();
  ProveMissingContinuationTokenRefuses();
  ProveOverWideRowRefuses();
  ProveCancellationAndBackpressure();
  ProveDisabledPathBoundedLegacyAndRefusal();
  ProveUnsafeAuthorityRefused();
  return 0;
}
