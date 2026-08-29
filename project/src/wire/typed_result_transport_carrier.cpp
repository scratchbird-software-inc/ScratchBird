// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "typed_result_transport_carrier.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::wire {
namespace {

constexpr const char* kFrameInvalid =
    "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID";
constexpr const char* kResourceLimit =
    "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED";
constexpr const char* kDatatypeInvalid = "DATATYPE.DESCRIPTOR_INVALID";
constexpr const char* kConnectionMismatch =
    "PARSER_SERVER_IPC.CONNECTION_MISMATCH";
constexpr const char* kSequenceInvalid =
    "PARSER_SERVER_IPC.SEQUENCE_INVALID";

bool UuidPresent(const TypedResultUuid& uuid) {
  return std::any_of(uuid.begin(), uuid.end(),
                     [](byte value) { return value != 0; });
}

enum class ComponentShape {
  empty,
  complete,
  partial,
};

ComponentShape QueryHandleShape(const TypedResultQueryHandleV1& handle) {
  const std::array<bool, 4> present{
      UuidPresent(handle.execution_uuid), UuidPresent(handle.result_set_uuid),
      UuidPresent(handle.row_descriptor_uuid), UuidPresent(handle.snapshot_uuid)};
  const auto count = std::count(present.begin(), present.end(), true);
  if (count == 0) return ComponentShape::empty;
  return count == present.size() ? ComponentShape::complete
                                 : ComponentShape::partial;
}

ComponentShape CursorDescriptorShape(
    const TypedResultCursorStreamDescriptorV1& descriptor) {
  const bool uuid = UuidPresent(descriptor.descriptor_uuid);
  const bool version = descriptor.descriptor_version != 0;
  const bool generation = descriptor.descriptor_generation != 0;
  const bool rows = descriptor.maximum_chunk_rows != 0;
  const bool bytes = descriptor.maximum_chunk_bytes != 0;
  const std::array<bool, 5> present{uuid, version, generation, rows, bytes};
  const auto count = std::count(present.begin(), present.end(), true);
  if (count == 0) return ComponentShape::empty;
  if (count == present.size() && descriptor.descriptor_version == 1) {
    return ComponentShape::complete;
  }
  return ComponentShape::partial;
}

TypedResultCarrierValidationResult CarrierError(
    TypedResultCodecStatus status,
    std::string diagnostic_code,
    std::string detail) {
  TypedResultCarrierValidationResult result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic_code);
  result.detail = std::move(detail);
  return result;
}

TypedResultCarrierValidationResult DescriptorCodecError(
    const TypedResultDescriptorCodecResult& decoded) {
  return CarrierError(decoded.status, decoded.diagnostic_code,
                      "result_descriptor_vector:" + decoded.detail);
}

TypedResultCarrierValidationResult BatchCodecError(
    const TypedResultBatchCodecResult& decoded) {
  return CarrierError(decoded.status, decoded.diagnostic_code,
                      "row_data_packet:" + decoded.detail);
}

TypedResultCarrierValidationResult DecodeAndAuthorizeDescriptor(
    const std::vector<byte>& encoded,
    u64 maximum_bytes,
    const TypedResultDescriptorAuthorityValidator& descriptor_authority) {
  if (maximum_bytes == 0 ||
      maximum_bytes > kTypedResultCarrierMaximumBytes ||
      encoded.size() > maximum_bytes) {
    return CarrierError(TypedResultCodecStatus::resource_limit_exceeded,
                        kResourceLimit,
                        "result_descriptor_vector_exceeds_effective_limit");
  }
  const auto decoded = DecodeTypedResultRowDescriptor(encoded);
  if (!decoded.ok()) return DescriptorCodecError(decoded);
  if (!descriptor_authority) {
    return CarrierError(TypedResultCodecStatus::descriptor_invalid,
                        kDatatypeInvalid,
                        "datatype_descriptor_authority_validator_required");
  }
  const auto authority = descriptor_authority(decoded.descriptor);
  if (!authority.accepted) {
    return CarrierError(
        TypedResultCodecStatus::descriptor_invalid,
        authority.diagnostic_code.empty() ? kDatatypeInvalid
                                          : authority.diagnostic_code,
        authority.detail.empty() ? "datatype_descriptor_authority_refused"
                                 : authority.detail);
  }
  TypedResultCarrierValidationResult result;
  result.row_descriptor = decoded.descriptor;
  return result;
}

bool TransactionIdentityCoherent(const TypedResultExecuteCarrierV1& carrier) {
  return UuidPresent(carrier.transaction_uuid) ==
         (carrier.local_transaction_id != 0);
}

bool FailureOutcome(TypedResultExecuteOutcome outcome) {
  return outcome == TypedResultExecuteOutcome::cancelled ||
         outcome == TypedResultExecuteOutcome::rejected ||
         outcome == TypedResultExecuteOutcome::unknown_outcome ||
         outcome == TypedResultExecuteOutcome::error;
}

bool MessageRequired(TypedResultExecuteOutcome outcome) {
  return outcome == TypedResultExecuteOutcome::rejected ||
         outcome == TypedResultExecuteOutcome::unknown_outcome ||
         outcome == TypedResultExecuteOutcome::error;
}

bool KnownOutcome(TypedResultExecuteOutcome outcome) {
  switch (outcome) {
    case TypedResultExecuteOutcome::complete:
    case TypedResultExecuteOutcome::cursor_open:
    case TypedResultExecuteOutcome::row_batch:
    case TypedResultExecuteOutcome::cancelled:
    case TypedResultExecuteOutcome::rejected:
    case TypedResultExecuteOutcome::unknown_outcome:
    case TypedResultExecuteOutcome::error:
      return true;
  }
  return false;
}

}  // namespace

TypedResultCarrierValidationResult ValidateTypedResultExecuteCarrierV1(
    const TypedResultExecuteRequestAuthorityV1& request_authority,
    const TypedResultExecuteCarrierV1& carrier,
    const TypedResultDescriptorAuthorityValidator& descriptor_authority) {
  if (!UuidPresent(request_authority.expected_server_request_uuid) ||
      !UuidPresent(carrier.server_request_uuid) ||
      request_authority.expected_server_request_uuid !=
          carrier.server_request_uuid) {
    return CarrierError(TypedResultCodecStatus::cursor_mismatch,
                        kConnectionMismatch,
                        "execute_server_request_identity_mismatch");
  }
  if (!KnownOutcome(carrier.outcome) ||
      !TransactionIdentityCoherent(carrier)) {
    return CarrierError(TypedResultCodecStatus::malformed_frame,
                        kFrameInvalid,
                        "execute_outcome_or_transaction_identity_invalid");
  }
  if (request_authority.maximum_descriptor_bytes == 0 ||
      request_authority.maximum_descriptor_bytes >
          kTypedResultCarrierMaximumBytes ||
      request_authority.maximum_row_packet_bytes == 0 ||
      request_authority.maximum_row_packet_bytes >
          kTypedResultCarrierMaximumBytes) {
    return CarrierError(TypedResultCodecStatus::resource_limit_exceeded,
                        kResourceLimit,
                        "execute_effective_result_limit_invalid");
  }
  if (carrier.result_descriptor_vector.size() >
          request_authority.maximum_descriptor_bytes ||
      carrier.row_data_packet.size() >
          request_authority.maximum_row_packet_bytes) {
    return CarrierError(TypedResultCodecStatus::resource_limit_exceeded,
                        kResourceLimit,
                        "execute_result_component_exceeds_effective_limit");
  }

  const auto query_shape = QueryHandleShape(carrier.query_handle);
  const auto cursor_descriptor_shape =
      CursorDescriptorShape(carrier.cursor_stream_descriptor);
  const bool cursor_present = UuidPresent(carrier.cursor_uuid);
  const bool descriptor_present = !carrier.result_descriptor_vector.empty();
  const bool packet_present = !carrier.row_data_packet.empty();
  const bool row_bearing =
      carrier.outcome == TypedResultExecuteOutcome::cursor_open ||
      carrier.outcome == TypedResultExecuteOutcome::row_batch;
  const bool token_required = request_authority.finality_sensitive ||
                              carrier.outcome ==
                                  TypedResultExecuteOutcome::unknown_outcome;

  if ((token_required && carrier.finality_token.empty()) ||
      (!token_required && !carrier.finality_token.empty()) ||
      (MessageRequired(carrier.outcome) &&
       carrier.message_vector_set.empty())) {
    return CarrierError(TypedResultCodecStatus::shape_invalid,
                        kFrameInvalid,
                        "execute_finality_or_message_matrix_invalid");
  }

  if (carrier.outcome == TypedResultExecuteOutcome::cursor_open) {
    if (!cursor_present || carrier.row_count != 0 || !descriptor_present ||
        packet_present || query_shape != ComponentShape::complete ||
        cursor_descriptor_shape != ComponentShape::complete ||
        carrier.cursor_stream_descriptor.maximum_chunk_rows >
            kTypedResultCarrierMaximumRows ||
        carrier.cursor_stream_descriptor.maximum_chunk_bytes >
            kTypedResultCarrierMaximumBytes) {
      return CarrierError(TypedResultCodecStatus::shape_invalid,
                          kFrameInvalid,
                          "execute_cursor_open_component_matrix_invalid");
    }
  } else if (carrier.outcome == TypedResultExecuteOutcome::row_batch) {
    if (cursor_present || carrier.row_count == 0 ||
        carrier.row_count > kTypedResultCarrierMaximumRows ||
        !descriptor_present || !packet_present ||
        query_shape != ComponentShape::complete ||
        cursor_descriptor_shape != ComponentShape::empty) {
      return CarrierError(TypedResultCodecStatus::shape_invalid,
                          kFrameInvalid,
                          "execute_row_batch_component_matrix_invalid");
    }
  } else {
    if (cursor_present || descriptor_present || packet_present ||
        query_shape != ComponentShape::empty ||
        cursor_descriptor_shape != ComponentShape::empty ||
        (FailureOutcome(carrier.outcome) && carrier.row_count != 0)) {
      return CarrierError(TypedResultCodecStatus::shape_invalid,
                          kFrameInvalid,
                          "execute_non_row_component_matrix_invalid");
    }
    return {};
  }

  auto descriptor = DecodeAndAuthorizeDescriptor(
      carrier.result_descriptor_vector,
      request_authority.maximum_descriptor_bytes, descriptor_authority);
  if (!descriptor.ok()) return descriptor;
  if (descriptor.row_descriptor.descriptor_uuid !=
      carrier.query_handle.row_descriptor_uuid) {
    return CarrierError(TypedResultCodecStatus::descriptor_mismatch,
                        kDatatypeInvalid,
                        "outer_query_handle_row_descriptor_mismatch");
  }

  if (carrier.outcome == TypedResultExecuteOutcome::cursor_open) {
    TypedResultCarrierValidationResult result;
    result.row_descriptor = descriptor.row_descriptor;
    result.cursor_state.initialized = true;
    result.cursor_state.cursor_uuid = carrier.cursor_uuid;
    result.cursor_state.cursor_stream_descriptor =
        carrier.cursor_stream_descriptor;
    result.cursor_state.query_handle = carrier.query_handle;
    result.cursor_state.encoded_row_descriptor =
        carrier.result_descriptor_vector;
    result.cursor_state.row_descriptor = descriptor.row_descriptor;
    return result;
  }

  TypedResultCarrierBinding binding;
  binding.kind = TypedResultCarrierKind::ps_execute_result_v1;
  binding.row_count = carrier.row_count;
  binding.end_of_rowset = true;
  binding.execution_uuid = carrier.query_handle.execution_uuid;
  binding.result_set_uuid = carrier.query_handle.result_set_uuid;
  binding.snapshot_uuid = carrier.query_handle.snapshot_uuid;
  const auto batch = DecodeTypedResultBatch(
      carrier.row_data_packet, descriptor.row_descriptor, binding);
  if (!batch.ok()) return BatchCodecError(batch);
  TypedResultCarrierValidationResult result;
  result.row_descriptor = descriptor.row_descriptor;
  result.batch = batch.batch;
  return result;
}

TypedResultCarrierValidationResult ValidateTypedResultFetchCarrierV1(
    const TypedResultFetchRequestAuthorityV1& request_authority,
    const TypedResultFetchCarrierV1& carrier,
    const TypedResultCursorCarrierStateV1& cursor_state) {
  const auto fail = [&](TypedResultCodecStatus status,
                        std::string diagnostic_code,
                        std::string detail) {
    auto result = CarrierError(status, std::move(diagnostic_code),
                               std::move(detail));
    result.cursor_state = cursor_state;
    return result;
  };

  if (!cursor_state.initialized || cursor_state.terminal) {
    return fail(TypedResultCodecStatus::sequence_mismatch,
                kSequenceInvalid,
                cursor_state.terminal ? "fetch_after_terminal_cursor"
                                      : "fetch_cursor_state_not_initialized");
  }
  if (QueryHandleShape(cursor_state.query_handle) !=
          ComponentShape::complete ||
      CursorDescriptorShape(cursor_state.cursor_stream_descriptor) !=
          ComponentShape::complete ||
      cursor_state.encoded_row_descriptor.empty() ||
      cursor_state.row_descriptor.descriptor_uuid !=
          cursor_state.query_handle.row_descriptor_uuid) {
    return fail(TypedResultCodecStatus::descriptor_invalid,
                kDatatypeInvalid,
                "live_cursor_typed_result_state_invalid");
  }
  if (request_authority.direction != TypedResultFetchDirection::forward ||
      request_authority.timeout_millis == 0 ||
      request_authority.maximum_rows == 0 ||
      request_authority.maximum_bytes == 0 ||
      request_authority.maximum_rows >
          cursor_state.cursor_stream_descriptor.maximum_chunk_rows ||
      request_authority.maximum_rows > kTypedResultCarrierMaximumRows ||
      request_authority.maximum_bytes >
          cursor_state.cursor_stream_descriptor.maximum_chunk_bytes ||
      request_authority.maximum_bytes > kTypedResultCarrierMaximumBytes) {
    return fail(TypedResultCodecStatus::resource_limit_exceeded,
                kResourceLimit,
                "fetch_request_exceeds_live_cursor_bounds");
  }
  if (!UuidPresent(request_authority.cursor_uuid) ||
      request_authority.cursor_uuid != cursor_state.cursor_uuid ||
      request_authority.cursor_stream_descriptor_uuid !=
          cursor_state.cursor_stream_descriptor.descriptor_uuid ||
      request_authority.cursor_stream_descriptor_version !=
          cursor_state.cursor_stream_descriptor.descriptor_version ||
      request_authority.cursor_stream_descriptor_generation !=
          cursor_state.cursor_stream_descriptor.descriptor_generation ||
      carrier.cursor_uuid != cursor_state.cursor_uuid) {
    return fail(TypedResultCodecStatus::cursor_mismatch,
                kConnectionMismatch,
                "fetch_cursor_or_stream_descriptor_mismatch");
  }
  if (carrier.row_count > request_authority.maximum_rows ||
      carrier.row_count > kTypedResultCarrierMaximumRows ||
      carrier.row_data_packet.size() > request_authority.maximum_bytes ||
      carrier.row_data_packet.size() > kTypedResultCarrierMaximumBytes) {
    return fail(TypedResultCodecStatus::resource_limit_exceeded,
                kResourceLimit,
                "fetch_result_exceeds_admitted_request_bounds");
  }
  if ((carrier.row_count == 0) != carrier.row_data_packet.empty()) {
    return fail(TypedResultCodecStatus::shape_invalid,
                kFrameInvalid,
                "fetch_row_count_and_packet_presence_mismatch");
  }

  auto next_state = cursor_state;
  if (carrier.row_count == 0) {
    next_state.terminal = carrier.end_of_cursor;
    TypedResultCarrierValidationResult result;
    result.row_descriptor = cursor_state.row_descriptor;
    result.cursor_state = std::move(next_state);
    return result;
  }

  TypedResultCarrierBinding binding;
  binding.kind = TypedResultCarrierKind::ps_fetch_result_v1;
  binding.row_count = carrier.row_count;
  binding.end_of_rowset = carrier.end_of_cursor;
  binding.execution_uuid = cursor_state.query_handle.execution_uuid;
  binding.result_set_uuid = cursor_state.query_handle.result_set_uuid;
  binding.snapshot_uuid = cursor_state.query_handle.snapshot_uuid;
  binding.cursor_uuid = cursor_state.cursor_uuid;
  binding.cursor_stream_descriptor_uuid =
      cursor_state.cursor_stream_descriptor.descriptor_uuid;
  binding.cursor_stream_descriptor_version =
      cursor_state.cursor_stream_descriptor.descriptor_version;
  binding.cursor_stream_descriptor_generation =
      cursor_state.cursor_stream_descriptor.descriptor_generation;

  const auto batch = DecodeTypedResultBatch(
      carrier.row_data_packet, cursor_state.row_descriptor, binding,
      &next_state.batch_state);
  if (!batch.ok()) {
    return fail(batch.status, batch.diagnostic_code,
                "row_data_packet:" + batch.detail);
  }
  next_state.terminal = carrier.end_of_cursor;
  TypedResultCarrierValidationResult result;
  result.row_descriptor = cursor_state.row_descriptor;
  result.batch = batch.batch;
  result.cursor_state = std::move(next_state);
  return result;
}

}  // namespace scratchbird::wire
