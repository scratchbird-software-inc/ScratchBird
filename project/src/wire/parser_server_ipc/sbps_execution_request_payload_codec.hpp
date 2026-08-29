// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Pure SBPS-PAYLOAD-TLV1 codecs for Core schemas 1042 and 1044.  Frame,
// endpoint/profile, authentication, and engine dispatch remain outside this
// component.  Decode accepts only authority facts established at those outer
// boundaries and never creates session, generation, cursor, or transaction
// authority from payload bytes.

#include "runtime_platform.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::parser::ipc {

using scratchbird::core::platform::byte;
using PsRequestUuidV1 = std::array<byte, 16>;

inline constexpr std::uint16_t kPsRequestPayloadLayoutRevisionV1 = 1;
inline constexpr std::uint16_t kPsExecuteRequestMessageTypeV1 = 42;
inline constexpr std::uint16_t kPsFetchRequestMessageTypeV1 = 44;
inline constexpr std::uint32_t kPsExecuteRequestSchemaIdV1 = 1042;
inline constexpr std::uint32_t kPsFetchRequestSchemaIdV1 = 1044;
inline constexpr std::uint64_t kPsRequestMaximumAssembledPayloadBytes =
    64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kPsRequestMaximumSblrBytes =
    8ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kPsRequestMaximumParameterPacketBytes =
    16ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kPsFetchMaximumRowsV1 = 1'048'576;
inline constexpr std::uint64_t kPsFetchMaximumBytesV1 =
    16ull * 1024ull * 1024ull;

enum class PsTransactionRequestKindV1 : std::uint8_t {
  join_existing = 1,
  autocommit_statement = 2,
  begin_explicit = 3,
  commit_existing = 4,
  rollback_existing = 5,
  create_savepoint = 6,
  release_savepoint = 7,
  rollback_to_savepoint = 8,
};

enum class PsExecutionResultModeV1 : std::uint8_t {
  no_rows = 1,
  single_row = 2,
  row_batch = 3,
  cursor = 4,
};

enum class PsExecutionCursorModeV1 : std::uint8_t {
  none = 1,
  forward_only = 2,
};

enum class PsFetchDirectionV1 : std::uint8_t {
  forward = 1,
};

struct PsTransactionRequestV1 {
  PsTransactionRequestKindV1 request_kind =
      PsTransactionRequestKindV1::join_existing;
  PsRequestUuidV1 transaction_uuid{};
  PsRequestUuidV1 savepoint_uuid{};
  PsRequestUuidV1 requested_isolation_profile_uuid{};
  PsRequestUuidV1 requested_sync_profile_uuid{};
};

struct PsExecutionOptionsV1 {
  PsExecutionResultModeV1 result_mode = PsExecutionResultModeV1::no_rows;
  PsExecutionCursorModeV1 cursor_mode = PsExecutionCursorModeV1::none;
  std::uint64_t max_rows = 0;
  std::uint64_t max_result_bytes = 0;
  std::uint64_t statement_timeout_ms = 0;
  PsRequestUuidV1 donor_execution_profile_uuid{};
  bool allow_partial_result = false;
};

struct PsExecuteRequestPayloadV1 {
  PsRequestUuidV1 session_uuid{};
  PsRequestUuidV1 prepared_statement_uuid{};
  std::vector<byte> sblr_envelope;
  std::vector<byte> parameter_data_packet;
  PsTransactionRequestV1 transaction_request;
  PsExecutionOptionsV1 execution_options;
  std::uint64_t catalog_generation = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_generation = 0;
};

struct PsExecuteRequestValidationContextV1 {
  PsRequestUuidV1 expected_session_uuid{};
  std::uint64_t expected_catalog_generation = 0;
  std::uint64_t expected_security_epoch = 0;
  std::uint64_t expected_policy_generation = 0;
  PsRequestUuidV1 admitted_donor_execution_profile_uuid{};
  std::uint32_t authoritative_parameter_count = 0;
  std::uint64_t maximum_sblr_bytes = kPsRequestMaximumSblrBytes;
  std::uint64_t maximum_parameter_packet_bytes =
      kPsRequestMaximumParameterPacketBytes;
  std::uint64_t maximum_assembled_payload_bytes =
      kPsRequestMaximumAssembledPayloadBytes;
};

struct PsFetchRequestPayloadV1 {
  PsRequestUuidV1 session_uuid{};
  PsRequestUuidV1 cursor_uuid{};
  std::uint64_t max_rows = 0;
  std::uint64_t max_bytes = 0;
  std::uint64_t timeout_ms = 0;
  PsFetchDirectionV1 fetch_direction = PsFetchDirectionV1::forward;
  PsRequestUuidV1 cursor_stream_descriptor_uuid{};
  std::uint16_t cursor_stream_descriptor_version = 0;
  std::uint64_t cursor_stream_descriptor_generation = 0;
};

struct PsFetchRequestValidationContextV1 {
  PsRequestUuidV1 expected_session_uuid{};
  PsRequestUuidV1 expected_cursor_uuid{};
  PsRequestUuidV1 expected_cursor_stream_descriptor_uuid{};
  std::uint16_t expected_cursor_stream_descriptor_version = 0;
  std::uint64_t expected_cursor_stream_descriptor_generation = 0;
  std::uint64_t maximum_rows = kPsFetchMaximumRowsV1;
  std::uint64_t maximum_bytes = kPsFetchMaximumBytesV1;
  // Zero means the endpoint policy does not impose a smaller codec-visible
  // ceiling.  The payload value itself is always non-zero.
  std::uint64_t maximum_timeout_ms = 0;
  std::uint64_t maximum_assembled_payload_bytes =
      kPsRequestMaximumAssembledPayloadBytes;
};

enum class PsRequestPayloadStatusV1 : std::uint8_t {
  ok = 0,
  invalid_argument,
  resource_limit_exceeded,
  frame_payload_invalid,
  request_semantics_invalid,
  request_authority_mismatch,
};

struct PsRequestPayloadDiagnosticV1 {
  PsRequestPayloadStatusV1 status =
      PsRequestPayloadStatusV1::invalid_argument;
  std::string diagnostic_code;
  std::uint16_t field_id = 0;
  std::string detail;

  [[nodiscard]] bool ok() const {
    return status == PsRequestPayloadStatusV1::ok;
  }
};

struct PsExecuteRequestPayloadCodecResultV1 {
  PsRequestPayloadDiagnosticV1 outcome;
  std::vector<byte> canonical_payload;
  PsExecuteRequestPayloadV1 request;

  [[nodiscard]] bool ok() const { return outcome.ok(); }
};

struct PsFetchRequestPayloadCodecResultV1 {
  PsRequestPayloadDiagnosticV1 outcome;
  std::vector<byte> canonical_payload;
  PsFetchRequestPayloadV1 request;

  [[nodiscard]] bool ok() const { return outcome.ok(); }
};

PsExecuteRequestPayloadCodecResultV1 EncodeAndValidatePsExecuteRequestV1Payload(
    const PsExecuteRequestPayloadV1& request,
    const PsExecuteRequestValidationContextV1& context);

PsExecuteRequestPayloadCodecResultV1 DecodeAndValidatePsExecuteRequestV1Payload(
    std::span<const byte> payload,
    const PsExecuteRequestValidationContextV1& context);

PsFetchRequestPayloadCodecResultV1 EncodeAndValidatePsFetchRequestV1Payload(
    const PsFetchRequestPayloadV1& request,
    const PsFetchRequestValidationContextV1& context);

PsFetchRequestPayloadCodecResultV1 DecodeAndValidatePsFetchRequestV1Payload(
    std::span<const byte> payload,
    const PsFetchRequestValidationContextV1& context);

const char* PsRequestPayloadStatusNameV1(PsRequestPayloadStatusV1 status);

}  // namespace scratchbird::parser::ipc
