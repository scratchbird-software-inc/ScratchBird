// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-WIRE-DIRECT-BINARY-RESULT-FRAME-ANCHOR
#include "result_batch_transfer.hpp"
#include "runtime_consumption_evidence.hpp"
#include "runtime_capabilities.hpp"
#include "observability/performance_metric_event.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::wire {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::engine::executor::ResultBatchLayoutKind;
using scratchbird::engine::executor::VectorizedResultBatch;

inline constexpr u32 kDirectBinaryResultFrameVersion = 1;

struct DirectBinaryResultFrameColumnLayout {
  std::string name;
  ResultBatchLayoutKind layout = ResultBatchLayoutKind::unknown;
  u64 row_count = 0;
  u64 null_count = 0;
  u64 redaction_count = 0;
  u64 validity_bitmap_offset = 0;
  u64 validity_bitmap_length = 0;
  u64 redaction_bitmap_offset = 0;
  u64 redaction_bitmap_length = 0;
  u64 payload_offset = 0;
  u64 payload_length = 0;
  u64 fixed_width_bytes = 0;
  u64 offset_count = 0;
  u64 dictionary_value_count = 0;
  u64 run_count = 0;
  u64 child_count = 0;
  u64 list_value_count = 0;
  std::vector<DirectBinaryResultFrameColumnLayout> children;
};

struct DirectBinaryResultFrameDescriptor {
  u32 version = kDirectBinaryResultFrameVersion;
  u64 row_count = 0;
  u64 column_count = 0;
  u64 payload_length = 0;
  std::vector<DirectBinaryResultFrameColumnLayout> columns;
};

struct DirectBinaryResultFrame {
  DirectBinaryResultFrameDescriptor descriptor;
  std::vector<std::uint8_t> bytes;
};

struct DirectBinaryResultFrameResult {
  Status status;
  bool fail_closed = false;
  DirectBinaryResultFrame frame;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct DirectBinaryResultFrameWindowPolicy {
  u64 start_row = 0;
  u64 requested_rows = 0;
  u64 max_rows = 0;
  u64 max_frame_bytes = 0;
  u64 client_credit_rows = 0;
  u64 client_credit_bytes = 0;
  u64 frame_sequence = 0;
  bool cancellation_requested = false;
  bool require_ordered_output = true;
};

struct DirectBinaryResultFrameWindowResult {
  Status status;
  bool fail_closed = false;
  DirectBinaryResultFrame frame;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;
  u64 frame_sequence = 0;
  u64 start_row = 0;
  u64 row_count = 0;
  u64 next_start_row = 0;
  u64 actual_frame_bytes = 0;
  bool continuation_required = false;
  bool cancelled = false;
  bool backpressure = false;
  bool ordering_preserved = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

// SEARCH_KEY: ORH_BINARY_BENCHMARK_RESULT_FAST_PATH
// Route-facing adapter for benchmark-clean result delivery. The direct binary
// frame remains data transport only and carries no parser, storage, visibility,
// or transaction finality authority.
struct BinaryResultFastPathObservation {
  std::string route_kind;
  std::string statement_family;
  bool benchmark_clean_candidate = false;
  const DirectBinaryResultFrameResult* frame_result = nullptr;
  scratchbird::engine::internal_api::InstrumentationOverheadPolicy
      instrumentation_policy;
  bool equivalent_result_materialization = false;
  bool support_evidence_available_outside_timed_path = false;
  bool disabled_or_fallback = false;
  std::string disabled_reason;
  bool parser_or_cache_executes_sql = false;
  bool parser_or_cache_owns_transaction_finality = false;
  std::string transaction_authority = "engine.mga.transaction_inventory";
  scratchbird::engine::optimizer::RuntimeOptimizedPathEvidence runtime_evidence;
  std::string result_contract_hash;
  std::string diagnostic_code;
};

DirectBinaryResultFrameResult BuildDirectBinaryResultFrame(
    const VectorizedResultBatch& batch);

DirectBinaryResultFrameWindowResult BuildDirectBinaryResultFrameWindow(
    const VectorizedResultBatch& batch,
    const DirectBinaryResultFrameWindowPolicy& policy);

DirectBinaryResultFrameResult ParseDirectBinaryResultFrame(
    const std::vector<std::uint8_t>& bytes);

DirectBinaryResultFrameResult ValidateDirectBinaryResultFrameRuntimeCompatibility(
    const scratchbird::core::platform::RuntimeCompatibilityDescriptor&
        descriptor);

DirectBinaryResultFrameResult ValidateDirectBinaryResultFrameEvidenceClaims(
    const std::vector<std::string>& evidence_claims);

const char* DirectBinaryResultFrameFieldVersion();
const char* DirectBinaryResultFrameCompressionPolicyFamilyName();
std::vector<std::string> DirectBinaryResultFrameCompressionPolicyEvidence();

scratchbird::engine::optimizer::BenchmarkResultFastPathEvidence
BuildBenchmarkResultFastPathEvidenceFromWireResult(
    const BinaryResultFastPathObservation& observation);

}  // namespace scratchbird::wire
