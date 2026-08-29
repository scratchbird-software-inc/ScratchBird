// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-WIRE-TYPED-RESULT-CARRIER-VALIDATION-ANCHOR
//
// Production-neutral validation for the exact ps_execute_result_v1 and
// ps_fetch_result_v1 typed-result component bindings.  The caller remains
// responsible for SBPS endpoint/profile/frame/session/connection admission.
// This layer accepts only authority facts obtained from that outer validation;
// descriptor or row-packet bytes never create those facts.

#include "typed_result_transport_codec.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace scratchbird::wire {

inline constexpr u64 kTypedResultCarrierMaximumRows = 1'048'576;
inline constexpr u64 kTypedResultCarrierMaximumBytes = 16u * 1024u * 1024u;

enum class TypedResultExecuteOutcome : std::uint8_t {
  complete = 1,
  cursor_open = 2,
  row_batch = 3,
  cancelled = 4,
  rejected = 5,
  unknown_outcome = 6,
  error = 7,
};

enum class TypedResultFetchDirection : std::uint8_t {
  forward = 1,
};

struct TypedResultQueryHandleV1 {
  TypedResultUuid execution_uuid{};
  TypedResultUuid result_set_uuid{};
  TypedResultUuid row_descriptor_uuid{};
  TypedResultUuid snapshot_uuid{};
};

struct TypedResultCursorStreamDescriptorV1 {
  TypedResultUuid descriptor_uuid{};
  u16 descriptor_version = 0;
  u64 descriptor_generation = 0;
  u64 maximum_chunk_rows = 0;
  u64 maximum_chunk_bytes = 0;
};

// These values come from the admitted request/frame context, not from the
// result payload being checked.
struct TypedResultExecuteRequestAuthorityV1 {
  TypedResultUuid expected_server_request_uuid{};
  bool finality_sensitive = false;
  u64 maximum_descriptor_bytes = kTypedResultCarrierMaximumBytes;
  u64 maximum_row_packet_bytes = kTypedResultCarrierMaximumBytes;
};

struct TypedResultExecuteCarrierV1 {
  TypedResultExecuteOutcome outcome = TypedResultExecuteOutcome::error;
  TypedResultUuid server_request_uuid{};
  TypedResultUuid transaction_uuid{};
  u64 local_transaction_id = 0;
  TypedResultUuid cursor_uuid{};
  u64 row_count = 0;
  std::vector<byte> result_descriptor_vector;
  std::vector<byte> row_data_packet;
  std::vector<byte> finality_token;
  std::vector<byte> message_vector_set;
  TypedResultCursorStreamDescriptorV1 cursor_stream_descriptor;
  TypedResultQueryHandleV1 query_handle;
};

struct TypedResultFetchRequestAuthorityV1 {
  TypedResultUuid cursor_uuid{};
  u64 maximum_rows = 0;
  u64 maximum_bytes = 0;
  u64 timeout_millis = 0;
  TypedResultFetchDirection direction = TypedResultFetchDirection::forward;
  TypedResultUuid cursor_stream_descriptor_uuid{};
  u16 cursor_stream_descriptor_version = 0;
  u64 cursor_stream_descriptor_generation = 0;
};

struct TypedResultFetchCarrierV1 {
  TypedResultUuid cursor_uuid{};
  u64 row_count = 0;
  std::vector<byte> row_data_packet;
  bool end_of_cursor = false;
  std::vector<byte> message_vector_set;
};

struct TypedResultDescriptorAuthorityDecision {
  bool accepted = false;
  std::string diagnostic_code;
  std::string detail;
};

// The production callback resolves every exact catalog/registry/descriptor
// tuple carried by the descriptor under its admitted snapshot.  An absent
// callback is a refusal; this API has no permissive default.
using TypedResultDescriptorAuthorityValidator = std::function<
    TypedResultDescriptorAuthorityDecision(const TypedResultRowDescriptor&)>;

struct TypedResultCursorCarrierStateV1 {
  bool initialized = false;
  bool terminal = false;
  TypedResultUuid cursor_uuid{};
  TypedResultCursorStreamDescriptorV1 cursor_stream_descriptor;
  TypedResultQueryHandleV1 query_handle;
  std::vector<byte> encoded_row_descriptor;
  TypedResultRowDescriptor row_descriptor;
  TypedResultCursorBatchState batch_state;
};

struct TypedResultCarrierValidationResult {
  TypedResultCodecStatus status = TypedResultCodecStatus::ok;
  std::string diagnostic_code;
  std::string detail;
  TypedResultRowDescriptor row_descriptor;
  TypedResultBatch batch;
  TypedResultCursorCarrierStateV1 cursor_state;

  [[nodiscard]] bool ok() const {
    return status == TypedResultCodecStatus::ok;
  }
};

TypedResultCarrierValidationResult ValidateTypedResultExecuteCarrierV1(
    const TypedResultExecuteRequestAuthorityV1& request_authority,
    const TypedResultExecuteCarrierV1& carrier,
    const TypedResultDescriptorAuthorityValidator& descriptor_authority);

// Validation is atomic: cursor_state is copied, and the returned state advances
// only after the complete packet and every cell pass.  The caller replaces its
// live state only on success.
TypedResultCarrierValidationResult ValidateTypedResultFetchCarrierV1(
    const TypedResultFetchRequestAuthorityV1& request_authority,
    const TypedResultFetchCarrierV1& carrier,
    const TypedResultCursorCarrierStateV1& cursor_state);

}  // namespace scratchbird::wire
