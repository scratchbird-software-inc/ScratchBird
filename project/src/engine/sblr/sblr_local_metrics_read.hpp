// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "sblr_engine_envelope.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

// IA10B-LOCAL-METRICS-READ-WIRE-V1
inline constexpr std::uint32_t kSblrLocalMetricsReadMaximumBytes = 384;

enum class SblrLocalMetricsQueryClass : std::uint8_t {
  registry = 1,
  current = 2,
  history = 3,
  rollup = 4,
};

struct SblrLocalMetricsReadRequest {
  SblrLocalMetricsQueryClass query_class = SblrLocalMetricsQueryClass::registry;
  std::uint32_t page_size = 1;
  std::string selector;
  std::uint64_t start_time_ns = 0;
  std::uint64_t end_time_ns = 0;
  std::uint64_t registry_epoch = 0;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> cursor_digest{};
};

struct SblrLocalMetricsReadCodecResult {
  bool ok = false;
  SblrLocalMetricsReadRequest request;
  std::vector<std::uint8_t> canonical_bytes;
  std::string sha256_hex;
  std::string diagnostic_id;
  std::string detail;
};

struct SblrLocalMetricsReadDispatchResult {
  bool accepted = false;
  SblrLocalMetricsReadRequest request;
  std::string diagnostic_id;
  std::string detail;
  std::vector<scratchbird::engine::internal_api::EngineEvidenceReference> evidence;
};

bool IsSblrLocalMetricsReadOperation(std::string_view operation_id) noexcept;
SblrLocalMetricsReadCodecResult EncodeSblrLocalMetricsReadRequest(
    const SblrLocalMetricsReadRequest& request);
SblrLocalMetricsReadCodecResult DecodeSblrLocalMetricsReadRequest(
    const std::uint8_t* data, std::size_t size);
SblrLocalMetricsReadCodecResult DecodeSblrLocalMetricsReadOperand(
    const SblrOperationEnvelope& envelope);
SblrOperand MakeSblrLocalMetricsReadOperand(
    const SblrLocalMetricsReadCodecResult& encoded);
SblrLocalMetricsReadDispatchResult DispatchSblrLocalMetricsRead(
    const SblrOperationEnvelope& envelope,
    const scratchbird::engine::internal_api::EngineRequestContext& context);

}  // namespace scratchbird::engine::sblr
