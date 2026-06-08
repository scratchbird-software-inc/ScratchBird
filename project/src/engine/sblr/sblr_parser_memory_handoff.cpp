// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-012 SBLR/parser handoff buffer backed by an explicit reserved resource.
#include "sblr_parser_memory_handoff.hpp"

#include "runtime_platform.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u64;

constexpr const char* kAnchor =
    "CEIC-012_QUERY_OPERATOR_PLANNER_PARSER_ARENAS";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::parser};
}

Status ErrorStatus(StatusCode code = StatusCode::memory_invalid_request) {
  return {code, Severity::error, Subsystem::parser};
}

std::string Digest(std::string_view payload) {
  u64 hash = 1469598103934665603ull;
  for (const unsigned char ch : payload) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "sblr-parser-handoff-v1:" << std::hex << std::setw(16)
      << std::setfill('0') << hash;
  return out.str();
}

SblrParserHandoffBufferResult Refuse(std::string operation_id,
                                     std::string code,
                                     std::string message,
                                     std::string reason,
                                     StatusCode status_code =
                                         StatusCode::memory_invalid_request) {
  SblrParserHandoffBufferResult result;
  result.status = ErrorStatus(status_code);
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(
      result.status.code,
      result.status.severity,
      result.status.subsystem,
      std::move(code),
      std::move(message),
      {{"operation_id", std::move(operation_id)}, {"reason", std::move(reason)}},
      {},
      "engine.sblr.parser_memory_handoff",
      "parser-to-SBLR handoff buffers require a CEIC-011 committed reservation-backed resource");
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("sblr.parser_handoff.fail_closed=true");
  result.evidence.push_back(
      "sblr.parser_handoff.authority_scope=translation_buffer_only_not_parser_execution_not_transaction_finality_visibility_recovery_donor_benchmark_cluster_or_optimizer_authority");
  return result;
}

}  // namespace

SblrParserHandoffBufferResult BuildSblrParserHandoffBuffer(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::string operation_id,
    std::string_view payload,
    bool engine_mga_authoritative,
    bool parser_or_donor_finality_authority,
    bool debug_or_relaxed_path) {
  if (resource == nullptr || !resource->active()) {
    return Refuse(std::move(operation_id),
                  "SB_CEIC_012_SBLR_HANDOFF.RESOURCE_REQUIRED",
                  "sblr.ceic_012.parser_handoff.resource_required",
                  "active_reservation_backed_resource_required");
  }
  if (operation_id.empty() || payload.empty()) {
    return Refuse(std::move(operation_id),
                  "SB_CEIC_012_SBLR_HANDOFF.REQUEST_INVALID",
                  "sblr.ceic_012.parser_handoff.request_invalid",
                  "operation_id_and_payload_required");
  }
  if (!engine_mga_authoritative || parser_or_donor_finality_authority ||
      debug_or_relaxed_path) {
    return Refuse(std::move(operation_id),
                  "SB_CEIC_012_SBLR_HANDOFF.UNSAFE_AUTHORITY",
                  "sblr.ceic_012.parser_handoff.unsafe_authority",
                  "engine_mga_required_and_parser_donor_debug_authority_refused");
  }

  scratchbird::core::memory::ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = static_cast<u64>(payload.size());
  allocation.alignment = alignof(char);
  allocation.purpose = "sblr.parser_handoff_buffer";
  const auto allocated = resource->Allocate(std::move(allocation));
  if (!allocated.ok()) {
    SblrParserHandoffBufferResult result;
    result.status = allocated.status;
    result.fail_closed = true;
    result.diagnostic = allocated.diagnostic;
    result.evidence.push_back(kAnchor);
    result.evidence.push_back("sblr.parser_handoff.fail_closed=true");
    result.evidence.push_back("sblr.parser_handoff.refused=allocation_refused");
    return result;
  }
  std::memcpy(allocated.pointer, payload.data(), payload.size());

  SblrParserHandoffBufferResult result;
  result.status = OkStatus();
  result.payload_bytes = static_cast<u64>(payload.size());
  result.digest = Digest(payload);
  result.evidence.push_back(kAnchor);
  result.evidence.push_back("sblr.parser_handoff.operation_id=" + operation_id);
  result.evidence.push_back("sblr.parser_handoff.resource_passed=true");
  result.evidence.push_back("sblr.parser_handoff.after_reservation=true");
  result.evidence.push_back("sblr.parser_handoff.payload_bytes=" +
                            std::to_string(result.payload_bytes));
  result.evidence.push_back("sblr.parser_handoff.digest=" + result.digest);
  result.evidence.push_back(
      "sblr.parser_handoff.authority_scope=translation_buffer_only_not_parser_execution_not_transaction_finality_visibility_recovery_donor_benchmark_cluster_or_optimizer_authority");
  return result;
}

}  // namespace scratchbird::engine::sblr
