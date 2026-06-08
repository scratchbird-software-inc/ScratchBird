// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ODFR_STREAMING_CURSOR_RESULT_FRAME_WINDOWING
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::wire {

constexpr const char* kStreamingResultWindowSearchKey =
    "ODFR_STREAMING_CURSOR_RESULT_FRAME_WINDOWING";

enum class StreamingResultSurface {
  kSql,
  kDocument,
  kSearch,
  kVector,
  kGraph,
  kTimeSeries
};

enum class StreamingResultWindowAction {
  kStreamingFrameWindow,
  kBackpressureWait,
  kCancelled,
  kBoundedLegacyFetch,
  kRefuse
};

struct StreamingResultWindowRequest {
  StreamingResultSurface surface = StreamingResultSurface::kSql;
  bool streaming_frames_enabled = true;
  std::uint64_t total_rows = 0;
  std::uint64_t row_width_bytes = 0;
  std::uint64_t requested_fetch_rows = 0;
  std::uint64_t max_window_rows = 0;
  std::uint64_t max_frame_bytes = 0;
  std::uint64_t memory_budget_bytes = 0;
  std::uint64_t fetch_offset = 0;
  bool continuation_token_present = false;
  bool server_cancel_requested = false;
  bool client_backpressure = false;
  std::uint64_t legacy_fetch_row_bound = 0;
  bool ordering_key_present = true;
  bool parser_or_donor_authority = false;
  bool client_visibility_or_finality_authority = false;
  bool write_ahead_or_finality_authority = false;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool benchmark_clean = true;
};

struct StreamingResultWindowDecision {
  bool accepted = false;
  bool fail_closed = true;
  StreamingResultWindowAction action = StreamingResultWindowAction::kRefuse;
  StreamingResultSurface surface = StreamingResultSurface::kSql;
  std::uint64_t window_rows = 0;
  std::uint64_t frame_sequence = 0;
  std::uint64_t frame_start_row = 0;
  std::uint64_t next_fetch_offset = 0;
  std::uint64_t estimated_frame_bytes = 0;
  bool continuation_required = false;
  bool ordering_preserved = false;
  bool exact_refusal = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
};

const char* StreamingResultSurfaceName(StreamingResultSurface surface);
const char* StreamingResultWindowActionName(StreamingResultWindowAction action);

StreamingResultWindowRequest DefaultStreamingResultWindowRequest(
    StreamingResultSurface surface);
StreamingResultWindowDecision EvaluateStreamingResultWindow(
    const StreamingResultWindowRequest& request);
std::vector<std::string> StreamingResultWindowEvidence(
    const StreamingResultWindowDecision& decision);
std::vector<std::string> ResultBatchTransferStreamingWindowEvidenceAdapter(
    const StreamingResultWindowDecision& decision);

}  // namespace scratchbird::wire
