// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Exact SBPS-PAYLOAD-TLV1 adapters for the Core-defined typed execute and
// fetch result schemas.  This boundary never accepts or returns a text row
// payload: the descriptor and row packet remain their canonical binary forms.

#include "typed_result_transport_carrier.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::parser::ipc {

using scratchbird::core::platform::byte;

inline constexpr std::uint16_t kPsTypedResultPayloadLayoutRevisionV1 = 1;
inline constexpr std::uint32_t kPsExecuteResultV1SchemaId = 1043;
inline constexpr std::uint32_t kPsFetchResultV1SchemaId = 1045;
inline constexpr std::uint64_t kPsTypedResultMaximumAssembledPayloadBytes =
    64ull * 1024ull * 1024ull;

enum class PsTypedResultPayloadStatusV1 : std::uint8_t {
  ok = 0,
  invalid_argument,
  resource_limit_exceeded,
  frame_payload_invalid,
  typed_result_invalid,
};

struct PsTypedResultPayloadDiagnosticV1 {
  PsTypedResultPayloadStatusV1 status =
      PsTypedResultPayloadStatusV1::invalid_argument;
  std::string diagnostic_code;
  std::uint16_t field_id = 0;
  std::string detail;

  [[nodiscard]] bool ok() const {
    return status == PsTypedResultPayloadStatusV1::ok;
  }
};

struct PsExecuteResultPayloadCodecResultV1 {
  PsTypedResultPayloadDiagnosticV1 outcome;
  std::vector<byte> canonical_payload;
  wire::TypedResultExecuteCarrierV1 carrier;
  wire::TypedResultCarrierValidationResult validated;

  [[nodiscard]] bool ok() const { return outcome.ok(); }
};

struct PsFetchResultPayloadCodecResultV1 {
  PsTypedResultPayloadDiagnosticV1 outcome;
  std::vector<byte> canonical_payload;
  wire::TypedResultFetchCarrierV1 carrier;
  wire::TypedResultCarrierValidationResult validated;

  [[nodiscard]] bool ok() const { return outcome.ok(); }
};

// Encoding validates the complete outer carrier and every typed component
// before returning bytes.  A refusal returns an empty payload and no carrier.
PsExecuteResultPayloadCodecResultV1 EncodeAndValidatePsExecuteResultV1Payload(
    const wire::TypedResultExecuteRequestAuthorityV1& request_authority,
    const wire::TypedResultExecuteCarrierV1& carrier,
    const wire::TypedResultDescriptorAuthorityValidator& descriptor_authority);

// Decoding validates the exact TLV field set and canonical re-encoding before
// publishing the carrier or typed descriptor/batch to the caller.
PsExecuteResultPayloadCodecResultV1 DecodeAndValidatePsExecuteResultV1Payload(
    std::span<const byte> payload,
    const wire::TypedResultExecuteRequestAuthorityV1& request_authority,
    const wire::TypedResultDescriptorAuthorityValidator& descriptor_authority);

// Fetch validation is fail-atomic.  The caller replaces its live cursor state
// only with `validated.cursor_state` from a successful result.
PsFetchResultPayloadCodecResultV1 EncodeAndValidatePsFetchResultV1Payload(
    const wire::TypedResultFetchRequestAuthorityV1& request_authority,
    const wire::TypedResultFetchCarrierV1& carrier,
    const wire::TypedResultCursorCarrierStateV1& prior_cursor_state);

PsFetchResultPayloadCodecResultV1 DecodeAndValidatePsFetchResultV1Payload(
    std::span<const byte> payload,
    const wire::TypedResultFetchRequestAuthorityV1& request_authority,
    const wire::TypedResultCursorCarrierStateV1& prior_cursor_state);

const char* PsTypedResultPayloadStatusNameV1(
    PsTypedResultPayloadStatusV1 status);

}  // namespace scratchbird::parser::ipc
