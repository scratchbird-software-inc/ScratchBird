// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbps_execution_request_payload_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace scratchbird::parser::ipc {
namespace {

constexpr std::size_t kFieldHeaderBytes = 6;
constexpr const char* kFrameInvalid =
    "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID";
constexpr const char* kResourceInvalid =
    "PARSER_SERVER_IPC.RESOURCE_LIMIT_INVALID";
constexpr const char* kResourceExceeded =
    "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED";
constexpr const char* kSessionMismatch =
    "PARSER_SERVER_IPC.SESSION_MISMATCH";
constexpr const char* kGenerationStale =
    "PARSER_SERVER_IPC.GENERATION_STALE";
constexpr const char* kProfileMismatch =
    "PARSER_SERVER_IPC.PARSER_PROFILE_MISMATCH";

PsRequestPayloadDiagnosticV1 Ok() {
  PsRequestPayloadDiagnosticV1 result;
  result.status = PsRequestPayloadStatusV1::ok;
  return result;
}

PsRequestPayloadDiagnosticV1 Error(PsRequestPayloadStatusV1 status,
                                    std::string diagnostic_code,
                                    std::uint16_t field_id,
                                    std::string detail) {
  PsRequestPayloadDiagnosticV1 result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic_code);
  result.field_id = field_id;
  result.detail = std::move(detail);
  return result;
}

PsRequestPayloadDiagnosticV1 FrameError(std::uint16_t field_id,
                                        std::string detail) {
  return Error(PsRequestPayloadStatusV1::frame_payload_invalid,
               kFrameInvalid, field_id, std::move(detail));
}

PsRequestPayloadDiagnosticV1 SemanticError(std::uint16_t field_id,
                                           std::string detail) {
  return Error(PsRequestPayloadStatusV1::request_semantics_invalid,
               kFrameInvalid, field_id, std::move(detail));
}

PsRequestPayloadDiagnosticV1 AuthorityError(const char* diagnostic,
                                            std::uint16_t field_id,
                                            std::string detail) {
  return Error(PsRequestPayloadStatusV1::request_authority_mismatch,
               diagnostic, field_id, std::move(detail));
}

PsRequestPayloadDiagnosticV1 ResourceError(std::uint16_t field_id,
                                           std::string detail) {
  return Error(PsRequestPayloadStatusV1::resource_limit_exceeded,
               kResourceExceeded, field_id, std::move(detail));
}

bool UuidPresent(const PsRequestUuidV1& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

void AppendU16(std::vector<byte>* output, std::uint16_t value) {
  output->push_back(static_cast<byte>(value & 0xffu));
  output->push_back(static_cast<byte>((value >> 8u) & 0xffu));
}

void AppendU32(std::vector<byte>* output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<byte>((value >> shift) & 0xffu));
  }
}

std::array<byte, 2> U16Bytes(std::uint16_t value) {
  return {static_cast<byte>(value & 0xffu),
          static_cast<byte>((value >> 8u) & 0xffu)};
}

std::array<byte, 8> U64Bytes(std::uint64_t value) {
  std::array<byte, 8> result{};
  for (unsigned shift = 0; shift < 64; shift += 8) {
    result[shift / 8] = static_cast<byte>((value >> shift) & 0xffu);
  }
  return result;
}

std::uint16_t LoadU16(const byte* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8u);
}

std::uint32_t LoadU32(const byte* data) {
  std::uint32_t result = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    result |= static_cast<std::uint32_t>(data[shift / 8]) << shift;
  }
  return result;
}

std::uint64_t LoadU64(const byte* data) {
  std::uint64_t result = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    result |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
  }
  return result;
}

bool AddWithinLimit(std::size_t current,
                    std::size_t increment,
                    std::uint64_t maximum,
                    std::size_t* result) {
  if (result == nullptr ||
      increment > std::numeric_limits<std::size_t>::max() - current) {
    return false;
  }
  *result = current + increment;
  return *result <= maximum;
}

bool AppendField(std::vector<byte>* output,
                 std::uint16_t field_id,
                 std::span<const byte> value,
                 std::uint64_t maximum) {
  if (output == nullptr || field_id == 0 ||
      value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  std::size_t next = 0;
  if (!AddWithinLimit(output->size(), kFieldHeaderBytes, maximum, &next) ||
      !AddWithinLimit(next, value.size(), maximum, &next)) {
    return false;
  }
  AppendU16(output, field_id);
  AppendU32(output, static_cast<std::uint32_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
  return true;
}

template <std::size_t N>
bool AppendField(std::vector<byte>* output,
                 std::uint16_t field_id,
                 const std::array<byte, N>& value,
                 std::uint64_t maximum) {
  return AppendField(output, field_id,
                     std::span<const byte>(value.data(), value.size()), maximum);
}

bool AppendField(std::vector<byte>* output,
                 std::uint16_t field_id,
                 const std::vector<byte>& value,
                 std::uint64_t maximum) {
  return AppendField(output, field_id,
                     std::span<const byte>(value.data(), value.size()), maximum);
}

class FieldReader {
 public:
  explicit FieldReader(std::span<const byte> payload)
      : payload_(payload), offset_(2) {}

  bool Read(std::uint16_t expected_id,
            std::span<const byte>* value,
            PsRequestPayloadDiagnosticV1* error) {
    if (value == nullptr || error == nullptr) return false;
    if (offset_ > payload_.size() ||
        payload_.size() - offset_ < kFieldHeaderBytes) {
      *error = FrameError(expected_id, "required_field_missing_or_truncated");
      return false;
    }
    const auto actual_id = LoadU16(payload_.data() + offset_);
    const auto length = LoadU32(payload_.data() + offset_ + 2);
    if (actual_id != expected_id) {
      *error = FrameError(
          expected_id,
          "field_id_not_strictly_ascending_expected_" +
              std::to_string(expected_id) + "_actual_" +
              std::to_string(actual_id));
      return false;
    }
    offset_ += kFieldHeaderBytes;
    if (length > payload_.size() - offset_) {
      *error = FrameError(expected_id, "field_extent_exceeds_payload");
      return false;
    }
    *value = payload_.subspan(offset_, length);
    offset_ += length;
    return true;
  }

  [[nodiscard]] bool complete() const { return offset_ == payload_.size(); }

 private:
  std::span<const byte> payload_;
  std::size_t offset_;
};

bool BeginDecode(std::span<const byte> payload,
                 std::uint64_t maximum,
                 PsRequestPayloadDiagnosticV1* error) {
  if (error == nullptr) return false;
  if (payload.size() > maximum) {
    *error = ResourceError(0, "payload_exceeds_admitted_assembled_limit");
    return false;
  }
  if (payload.size() < 2) {
    *error = FrameError(0, "layout_revision_missing");
    return false;
  }
  if (LoadU16(payload.data()) != kPsRequestPayloadLayoutRevisionV1) {
    *error = FrameError(0, "layout_revision_invalid");
    return false;
  }
  return true;
}

bool PrimitiveSize(std::span<const byte> value,
                   std::size_t expected,
                   std::uint16_t field_id,
                   PsRequestPayloadDiagnosticV1* error) {
  if (value.size() == expected) return true;
  if (error != nullptr) {
    *error = FrameError(field_id, "primitive_field_length_invalid");
  }
  return false;
}

PsRequestUuidV1 LoadUuid(std::span<const byte> value) {
  PsRequestUuidV1 result{};
  std::copy(value.begin(), value.end(), result.begin());
  return result;
}

bool WorkBearing(PsTransactionRequestKindV1 kind) {
  return kind == PsTransactionRequestKindV1::join_existing ||
         kind == PsTransactionRequestKindV1::autocommit_statement;
}

bool ValidTransactionKind(PsTransactionRequestKindV1 kind) {
  const auto code = static_cast<std::uint8_t>(kind);
  return code >= static_cast<std::uint8_t>(
                     PsTransactionRequestKindV1::join_existing) &&
         code <= static_cast<std::uint8_t>(
                     PsTransactionRequestKindV1::rollback_to_savepoint);
}

bool ValidResultMode(PsExecutionResultModeV1 mode) {
  const auto code = static_cast<std::uint8_t>(mode);
  return code >= static_cast<std::uint8_t>(PsExecutionResultModeV1::no_rows) &&
         code <= static_cast<std::uint8_t>(PsExecutionResultModeV1::cursor);
}

bool ValidCursorMode(PsExecutionCursorModeV1 mode) {
  return mode == PsExecutionCursorModeV1::none ||
         mode == PsExecutionCursorModeV1::forward_only;
}

PsRequestPayloadDiagnosticV1 ValidateExecuteContext(
    const PsExecuteRequestValidationContextV1& context) {
  if (!UuidPresent(context.expected_session_uuid) ||
      context.expected_catalog_generation == 0 ||
      context.expected_security_epoch == 0 ||
      context.expected_policy_generation == 0) {
    return Error(PsRequestPayloadStatusV1::invalid_argument,
                 kFrameInvalid, 0,
                 "execute_validation_context_is_incomplete");
  }
  if (context.maximum_sblr_bytes == 0 ||
      context.maximum_sblr_bytes > kPsRequestMaximumSblrBytes ||
      context.maximum_parameter_packet_bytes == 0 ||
      context.maximum_parameter_packet_bytes >
          kPsRequestMaximumParameterPacketBytes ||
      context.maximum_assembled_payload_bytes == 0 ||
      context.maximum_assembled_payload_bytes >
          kPsRequestMaximumAssembledPayloadBytes) {
    return Error(PsRequestPayloadStatusV1::invalid_argument,
                 kResourceInvalid, 0,
                 "execute_validation_limits_are_outside_core_bounds");
  }
  return Ok();
}

PsRequestPayloadDiagnosticV1 ValidateFetchContext(
    const PsFetchRequestValidationContextV1& context) {
  if (!UuidPresent(context.expected_session_uuid) ||
      !UuidPresent(context.expected_cursor_uuid) ||
      !UuidPresent(context.expected_cursor_stream_descriptor_uuid) ||
      context.expected_cursor_stream_descriptor_version != 1 ||
      context.expected_cursor_stream_descriptor_generation == 0) {
    return Error(PsRequestPayloadStatusV1::invalid_argument,
                 kFrameInvalid, 0,
                 "fetch_validation_context_is_incomplete");
  }
  if (context.maximum_rows == 0 ||
      context.maximum_rows > kPsFetchMaximumRowsV1 ||
      context.maximum_bytes == 0 ||
      context.maximum_bytes > kPsFetchMaximumBytesV1 ||
      context.maximum_assembled_payload_bytes == 0 ||
      context.maximum_assembled_payload_bytes >
          kPsRequestMaximumAssembledPayloadBytes) {
    return Error(PsRequestPayloadStatusV1::invalid_argument,
                 kResourceInvalid, 0,
                 "fetch_validation_limits_are_outside_core_bounds");
  }
  return Ok();
}

PsRequestPayloadDiagnosticV1 ValidateTransactionRequest(
    const PsTransactionRequestV1& transaction) {
  if (!ValidTransactionKind(transaction.request_kind)) {
    return SemanticError(5, "transaction_request_kind_invalid");
  }
  const bool transaction_present = UuidPresent(transaction.transaction_uuid);
  const bool savepoint_present = UuidPresent(transaction.savepoint_uuid);
  const bool isolation_present =
      UuidPresent(transaction.requested_isolation_profile_uuid);
  const bool sync_present = UuidPresent(transaction.requested_sync_profile_uuid);
  switch (transaction.request_kind) {
    case PsTransactionRequestKindV1::join_existing:
      if (!transaction_present || savepoint_present || isolation_present ||
          sync_present) {
        return SemanticError(5, "join_existing_selector_matrix_invalid");
      }
      break;
    case PsTransactionRequestKindV1::autocommit_statement:
    case PsTransactionRequestKindV1::begin_explicit:
      if (transaction_present || savepoint_present) {
        return SemanticError(5,
                             "new_transaction_selector_matrix_invalid");
      }
      break;
    case PsTransactionRequestKindV1::commit_existing:
    case PsTransactionRequestKindV1::rollback_existing:
      if (!transaction_present || savepoint_present || isolation_present ||
          sync_present) {
        return SemanticError(5,
                             "transaction_completion_selector_matrix_invalid");
      }
      break;
    case PsTransactionRequestKindV1::create_savepoint:
    case PsTransactionRequestKindV1::release_savepoint:
    case PsTransactionRequestKindV1::rollback_to_savepoint:
      if (!transaction_present || !savepoint_present || isolation_present ||
          sync_present) {
        return SemanticError(5, "savepoint_selector_matrix_invalid");
      }
      break;
  }
  return Ok();
}

PsRequestPayloadDiagnosticV1 ValidateExecutionOptions(
    const PsExecutionOptionsV1& options,
    const PsExecuteRequestValidationContextV1& context) {
  if (!ValidResultMode(options.result_mode) ||
      !ValidCursorMode(options.cursor_mode)) {
    return SemanticError(6, "execution_option_enum_invalid");
  }
  if ((options.result_mode == PsExecutionResultModeV1::cursor) !=
      (options.cursor_mode == PsExecutionCursorModeV1::forward_only)) {
    return SemanticError(6, "result_cursor_mode_matrix_invalid");
  }
  if (options.allow_partial_result) {
    return SemanticError(6, "allow_partial_result_must_be_false");
  }
  if (UuidPresent(options.donor_execution_profile_uuid) &&
      options.donor_execution_profile_uuid !=
          context.admitted_donor_execution_profile_uuid) {
    return AuthorityError(kProfileMismatch, 6,
                          "donor_execution_profile_not_admitted");
  }
  return Ok();
}

PsRequestPayloadDiagnosticV1 ValidateExecuteRequest(
    const PsExecuteRequestPayloadV1& request,
    const PsExecuteRequestValidationContextV1& context) {
  auto validation = ValidateExecuteContext(context);
  if (!validation.ok()) return validation;
  if (!UuidPresent(request.session_uuid) ||
      request.session_uuid != context.expected_session_uuid) {
    return AuthorityError(kSessionMismatch, 1,
                          "execute_payload_session_does_not_match_frame");
  }
  if (request.catalog_generation != context.expected_catalog_generation ||
      request.security_epoch != context.expected_security_epoch ||
      request.policy_generation != context.expected_policy_generation) {
    return AuthorityError(kGenerationStale, 7,
                          "execute_generation_tuple_is_not_current");
  }
  if (request.sblr_envelope.size() > context.maximum_sblr_bytes) {
    return ResourceError(3, "sblr_envelope_exceeds_admitted_limit");
  }
  if (request.parameter_data_packet.size() >
      context.maximum_parameter_packet_bytes) {
    return ResourceError(4,
                         "parameter_data_packet_exceeds_admitted_limit");
  }
  validation = ValidateTransactionRequest(request.transaction_request);
  if (!validation.ok()) return validation;
  validation = ValidateExecutionOptions(request.execution_options, context);
  if (!validation.ok()) return validation;

  const bool work_bearing = WorkBearing(request.transaction_request.request_kind);
  const bool prepared = UuidPresent(request.prepared_statement_uuid);
  const bool direct = !request.sblr_envelope.empty();
  if (work_bearing) {
    if (prepared == direct) {
      return SemanticError(
          2, "work_bearing_request_requires_exactly_one_execution_path");
    }
    const bool parameters_present = !request.parameter_data_packet.empty();
    if (parameters_present !=
        (context.authoritative_parameter_count != 0)) {
      return SemanticError(
          4, "parameter_packet_presence_disagrees_with_authoritative_shape");
    }
  } else if (prepared || direct || !request.parameter_data_packet.empty()) {
    return SemanticError(
        3, "standalone_transaction_control_carries_execution_bytes");
  }
  return Ok();
}

PsRequestPayloadDiagnosticV1 ValidateFetchRequest(
    const PsFetchRequestPayloadV1& request,
    const PsFetchRequestValidationContextV1& context) {
  auto validation = ValidateFetchContext(context);
  if (!validation.ok()) return validation;
  if (!UuidPresent(request.session_uuid) ||
      request.session_uuid != context.expected_session_uuid) {
    return AuthorityError(kSessionMismatch, 1,
                          "fetch_payload_session_does_not_match_frame");
  }
  if (!UuidPresent(request.cursor_uuid) ||
      request.cursor_uuid != context.expected_cursor_uuid ||
      !UuidPresent(request.cursor_stream_descriptor_uuid) ||
      request.cursor_stream_descriptor_uuid !=
          context.expected_cursor_stream_descriptor_uuid ||
      request.cursor_stream_descriptor_version !=
          context.expected_cursor_stream_descriptor_version ||
      request.cursor_stream_descriptor_generation !=
          context.expected_cursor_stream_descriptor_generation) {
    return AuthorityError(kFrameInvalid, 2,
                          "fetch_cursor_or_descriptor_authority_mismatch");
  }
  if (request.max_rows == 0 || request.max_rows > context.maximum_rows ||
      request.max_bytes == 0 || request.max_bytes > context.maximum_bytes ||
      request.timeout_ms == 0 ||
      (context.maximum_timeout_ms != 0 &&
       request.timeout_ms > context.maximum_timeout_ms)) {
    return ResourceError(3, "fetch_limit_is_zero_or_exceeds_admitted_bound");
  }
  if (request.fetch_direction != PsFetchDirectionV1::forward) {
    return SemanticError(6, "fetch_direction_must_be_forward");
  }
  return Ok();
}

std::vector<byte> EncodeTransactionRequest(
    const PsTransactionRequestV1& transaction) {
  constexpr std::uint64_t kNestedMaximum = 1024;
  std::vector<byte> encoded;
  encoded.reserve(97);
  AppendU16(&encoded, kPsRequestPayloadLayoutRevisionV1);
  const std::array<byte, 1> kind{
      static_cast<byte>(transaction.request_kind)};
  const bool appended =
      AppendField(&encoded, 1, kind, kNestedMaximum) &&
      AppendField(&encoded, 2, transaction.transaction_uuid, kNestedMaximum) &&
      AppendField(&encoded, 3, transaction.savepoint_uuid, kNestedMaximum) &&
      AppendField(&encoded, 4, transaction.requested_isolation_profile_uuid,
                  kNestedMaximum) &&
      AppendField(&encoded, 5, transaction.requested_sync_profile_uuid,
                  kNestedMaximum);
  return appended ? encoded : std::vector<byte>{};
}

std::vector<byte> EncodeExecutionOptions(
    const PsExecutionOptionsV1& options) {
  constexpr std::uint64_t kNestedMaximum = 1024;
  std::vector<byte> encoded;
  encoded.reserve(85);
  AppendU16(&encoded, kPsRequestPayloadLayoutRevisionV1);
  const std::array<byte, 1> result_mode{
      static_cast<byte>(options.result_mode)};
  const std::array<byte, 1> cursor_mode{
      static_cast<byte>(options.cursor_mode)};
  const auto max_rows = U64Bytes(options.max_rows);
  const auto max_result_bytes = U64Bytes(options.max_result_bytes);
  const auto timeout = U64Bytes(options.statement_timeout_ms);
  const std::array<byte, 1> partial{
      static_cast<byte>(options.allow_partial_result ? 1 : 0)};
  const bool appended =
      AppendField(&encoded, 1, result_mode, kNestedMaximum) &&
      AppendField(&encoded, 2, cursor_mode, kNestedMaximum) &&
      AppendField(&encoded, 3, max_rows, kNestedMaximum) &&
      AppendField(&encoded, 4, max_result_bytes, kNestedMaximum) &&
      AppendField(&encoded, 5, timeout, kNestedMaximum) &&
      AppendField(&encoded, 6, options.donor_execution_profile_uuid,
                  kNestedMaximum) &&
      AppendField(&encoded, 7, partial, kNestedMaximum);
  return appended ? encoded : std::vector<byte>{};
}

bool DecodeTransactionRequest(std::span<const byte> payload,
                              PsTransactionRequestV1* transaction,
                              PsRequestPayloadDiagnosticV1* error) {
  if (transaction == nullptr ||
      !BeginDecode(payload, 1024, error)) {
    return false;
  }
  FieldReader reader(payload);
  std::array<std::span<const byte>, 5> fields{};
  for (std::uint16_t id = 1; id <= fields.size(); ++id) {
    if (!reader.Read(id, &fields[id - 1], error)) return false;
  }
  if (!reader.complete()) {
    *error = FrameError(5, "transaction_request_has_trailing_fields");
    return false;
  }
  if (!PrimitiveSize(fields[0], 1, 5, error) ||
      !PrimitiveSize(fields[1], 16, 5, error) ||
      !PrimitiveSize(fields[2], 16, 5, error) ||
      !PrimitiveSize(fields[3], 16, 5, error) ||
      !PrimitiveSize(fields[4], 16, 5, error)) {
    return false;
  }
  PsTransactionRequestV1 decoded;
  decoded.request_kind =
      static_cast<PsTransactionRequestKindV1>(fields[0][0]);
  decoded.transaction_uuid = LoadUuid(fields[1]);
  decoded.savepoint_uuid = LoadUuid(fields[2]);
  decoded.requested_isolation_profile_uuid = LoadUuid(fields[3]);
  decoded.requested_sync_profile_uuid = LoadUuid(fields[4]);
  *transaction = decoded;
  return true;
}

bool DecodeExecutionOptions(std::span<const byte> payload,
                            PsExecutionOptionsV1* options,
                            PsRequestPayloadDiagnosticV1* error) {
  if (options == nullptr || !BeginDecode(payload, 1024, error)) return false;
  FieldReader reader(payload);
  std::array<std::span<const byte>, 7> fields{};
  for (std::uint16_t id = 1; id <= fields.size(); ++id) {
    if (!reader.Read(id, &fields[id - 1], error)) return false;
  }
  if (!reader.complete()) {
    *error = FrameError(6, "execution_options_has_trailing_fields");
    return false;
  }
  const std::array<std::size_t, 7> sizes{1, 1, 8, 8, 8, 16, 1};
  for (std::size_t index = 0; index < sizes.size(); ++index) {
    if (!PrimitiveSize(fields[index], sizes[index], 6, error)) return false;
  }
  if (fields[6][0] > 1) {
    *error = FrameError(6, "allow_partial_result_boolean_invalid");
    return false;
  }
  PsExecutionOptionsV1 decoded;
  decoded.result_mode =
      static_cast<PsExecutionResultModeV1>(fields[0][0]);
  decoded.cursor_mode =
      static_cast<PsExecutionCursorModeV1>(fields[1][0]);
  decoded.max_rows = LoadU64(fields[2].data());
  decoded.max_result_bytes = LoadU64(fields[3].data());
  decoded.statement_timeout_ms = LoadU64(fields[4].data());
  decoded.donor_execution_profile_uuid = LoadUuid(fields[5]);
  decoded.allow_partial_result = fields[6][0] == 1;
  *options = decoded;
  return true;
}

PsRequestPayloadDiagnosticV1 EncodeExecuteUnchecked(
    const PsExecuteRequestPayloadV1& request,
    const PsExecuteRequestValidationContextV1& context,
    std::vector<byte>* payload) {
  if (payload == nullptr) {
    return Error(PsRequestPayloadStatusV1::invalid_argument,
                 kFrameInvalid, 0, "execute_output_is_null");
  }
  const auto transaction = EncodeTransactionRequest(request.transaction_request);
  const auto options = EncodeExecutionOptions(request.execution_options);
  if (transaction.empty() || options.empty()) {
    return ResourceError(0, "nested_execute_component_encoding_failed");
  }
  const auto catalog = U64Bytes(request.catalog_generation);
  const auto security = U64Bytes(request.security_epoch);
  const auto policy = U64Bytes(request.policy_generation);
  std::vector<byte> encoded;
  AppendU16(&encoded, kPsRequestPayloadLayoutRevisionV1);
  const auto maximum = context.maximum_assembled_payload_bytes;
  const bool appended =
      AppendField(&encoded, 1, request.session_uuid, maximum) &&
      AppendField(&encoded, 2, request.prepared_statement_uuid, maximum) &&
      AppendField(&encoded, 3, request.sblr_envelope, maximum) &&
      AppendField(&encoded, 4, request.parameter_data_packet, maximum) &&
      AppendField(&encoded, 5, transaction, maximum) &&
      AppendField(&encoded, 6, options, maximum) &&
      AppendField(&encoded, 7, catalog, maximum) &&
      AppendField(&encoded, 8, security, maximum) &&
      AppendField(&encoded, 9, policy, maximum);
  if (!appended) {
    return ResourceError(0, "execute_payload_exceeds_assembled_limit");
  }
  *payload = std::move(encoded);
  return Ok();
}

PsRequestPayloadDiagnosticV1 EncodeFetchUnchecked(
    const PsFetchRequestPayloadV1& request,
    const PsFetchRequestValidationContextV1& context,
    std::vector<byte>* payload) {
  if (payload == nullptr) {
    return Error(PsRequestPayloadStatusV1::invalid_argument,
                 kFrameInvalid, 0, "fetch_output_is_null");
  }
  const auto max_rows = U64Bytes(request.max_rows);
  const auto max_bytes = U64Bytes(request.max_bytes);
  const auto timeout = U64Bytes(request.timeout_ms);
  const std::array<byte, 1> direction{
      static_cast<byte>(request.fetch_direction)};
  const auto descriptor_version =
      U16Bytes(request.cursor_stream_descriptor_version);
  const auto descriptor_generation =
      U64Bytes(request.cursor_stream_descriptor_generation);
  std::vector<byte> encoded;
  AppendU16(&encoded, kPsRequestPayloadLayoutRevisionV1);
  const auto maximum = context.maximum_assembled_payload_bytes;
  const bool appended =
      AppendField(&encoded, 1, request.session_uuid, maximum) &&
      AppendField(&encoded, 2, request.cursor_uuid, maximum) &&
      AppendField(&encoded, 3, max_rows, maximum) &&
      AppendField(&encoded, 4, max_bytes, maximum) &&
      AppendField(&encoded, 5, timeout, maximum) &&
      AppendField(&encoded, 6, direction, maximum) &&
      AppendField(&encoded, 7, request.cursor_stream_descriptor_uuid,
                  maximum) &&
      AppendField(&encoded, 8, descriptor_version, maximum) &&
      AppendField(&encoded, 9, descriptor_generation, maximum);
  if (!appended) {
    return ResourceError(0, "fetch_payload_exceeds_assembled_limit");
  }
  *payload = std::move(encoded);
  return Ok();
}

}  // namespace

PsExecuteRequestPayloadCodecResultV1 EncodeAndValidatePsExecuteRequestV1Payload(
    const PsExecuteRequestPayloadV1& request,
    const PsExecuteRequestValidationContextV1& context) {
  PsExecuteRequestPayloadCodecResultV1 result;
  result.outcome = ValidateExecuteRequest(request, context);
  if (!result.outcome.ok()) return result;
  result.outcome = EncodeExecuteUnchecked(request, context,
                                          &result.canonical_payload);
  if (!result.outcome.ok()) return result;
  result.request = request;
  return result;
}

PsExecuteRequestPayloadCodecResultV1 DecodeAndValidatePsExecuteRequestV1Payload(
    std::span<const byte> payload,
    const PsExecuteRequestValidationContextV1& context) {
  PsExecuteRequestPayloadCodecResultV1 result;
  result.outcome = ValidateExecuteContext(context);
  if (!result.outcome.ok() ||
      !BeginDecode(payload, context.maximum_assembled_payload_bytes,
                   &result.outcome)) {
    return result;
  }
  FieldReader reader(payload);
  std::array<std::span<const byte>, 9> fields{};
  for (std::uint16_t id = 1; id <= fields.size(); ++id) {
    if (!reader.Read(id, &fields[id - 1], &result.outcome)) return result;
  }
  if (!reader.complete()) {
    result.outcome = FrameError(0, "execute_payload_has_trailing_fields");
    return result;
  }
  const std::array<std::size_t, 9> fixed_sizes{16, 16, 0, 0, 0, 0, 8, 8, 8};
  for (std::size_t index = 0; index < fixed_sizes.size(); ++index) {
    if (fixed_sizes[index] != 0 &&
        !PrimitiveSize(fields[index], fixed_sizes[index],
                       static_cast<std::uint16_t>(index + 1),
                       &result.outcome)) {
      return result;
    }
  }
  PsExecuteRequestPayloadV1 decoded;
  decoded.session_uuid = LoadUuid(fields[0]);
  decoded.prepared_statement_uuid = LoadUuid(fields[1]);
  decoded.sblr_envelope.assign(fields[2].begin(), fields[2].end());
  decoded.parameter_data_packet.assign(fields[3].begin(), fields[3].end());
  if (!DecodeTransactionRequest(fields[4], &decoded.transaction_request,
                                &result.outcome) ||
      !DecodeExecutionOptions(fields[5], &decoded.execution_options,
                              &result.outcome)) {
    return result;
  }
  decoded.catalog_generation = LoadU64(fields[6].data());
  decoded.security_epoch = LoadU64(fields[7].data());
  decoded.policy_generation = LoadU64(fields[8].data());
  result.outcome = ValidateExecuteRequest(decoded, context);
  if (!result.outcome.ok()) return result;
  std::vector<byte> canonical;
  result.outcome = EncodeExecuteUnchecked(decoded, context, &canonical);
  if (!result.outcome.ok()) return result;
  if (!std::equal(canonical.begin(), canonical.end(), payload.begin(),
                  payload.end())) {
    result.outcome = FrameError(0, "execute_payload_is_not_canonical");
    return result;
  }
  result.canonical_payload = std::move(canonical);
  result.request = std::move(decoded);
  return result;
}

PsFetchRequestPayloadCodecResultV1 EncodeAndValidatePsFetchRequestV1Payload(
    const PsFetchRequestPayloadV1& request,
    const PsFetchRequestValidationContextV1& context) {
  PsFetchRequestPayloadCodecResultV1 result;
  result.outcome = ValidateFetchRequest(request, context);
  if (!result.outcome.ok()) return result;
  result.outcome = EncodeFetchUnchecked(request, context,
                                        &result.canonical_payload);
  if (!result.outcome.ok()) return result;
  result.request = request;
  return result;
}

PsFetchRequestPayloadCodecResultV1 DecodeAndValidatePsFetchRequestV1Payload(
    std::span<const byte> payload,
    const PsFetchRequestValidationContextV1& context) {
  PsFetchRequestPayloadCodecResultV1 result;
  result.outcome = ValidateFetchContext(context);
  if (!result.outcome.ok() ||
      !BeginDecode(payload, context.maximum_assembled_payload_bytes,
                   &result.outcome)) {
    return result;
  }
  FieldReader reader(payload);
  std::array<std::span<const byte>, 9> fields{};
  for (std::uint16_t id = 1; id <= fields.size(); ++id) {
    if (!reader.Read(id, &fields[id - 1], &result.outcome)) return result;
  }
  if (!reader.complete()) {
    result.outcome = FrameError(0, "fetch_payload_has_trailing_fields");
    return result;
  }
  const std::array<std::size_t, 9> sizes{16, 16, 8, 8, 8, 1, 16, 2, 8};
  for (std::size_t index = 0; index < sizes.size(); ++index) {
    if (!PrimitiveSize(fields[index], sizes[index],
                       static_cast<std::uint16_t>(index + 1),
                       &result.outcome)) {
      return result;
    }
  }
  PsFetchRequestPayloadV1 decoded;
  decoded.session_uuid = LoadUuid(fields[0]);
  decoded.cursor_uuid = LoadUuid(fields[1]);
  decoded.max_rows = LoadU64(fields[2].data());
  decoded.max_bytes = LoadU64(fields[3].data());
  decoded.timeout_ms = LoadU64(fields[4].data());
  decoded.fetch_direction = static_cast<PsFetchDirectionV1>(fields[5][0]);
  decoded.cursor_stream_descriptor_uuid = LoadUuid(fields[6]);
  decoded.cursor_stream_descriptor_version = LoadU16(fields[7].data());
  decoded.cursor_stream_descriptor_generation = LoadU64(fields[8].data());
  result.outcome = ValidateFetchRequest(decoded, context);
  if (!result.outcome.ok()) return result;
  std::vector<byte> canonical;
  result.outcome = EncodeFetchUnchecked(decoded, context, &canonical);
  if (!result.outcome.ok()) return result;
  if (!std::equal(canonical.begin(), canonical.end(), payload.begin(),
                  payload.end())) {
    result.outcome = FrameError(0, "fetch_payload_is_not_canonical");
    return result;
  }
  result.canonical_payload = std::move(canonical);
  result.request = std::move(decoded);
  return result;
}

const char* PsRequestPayloadStatusNameV1(PsRequestPayloadStatusV1 status) {
  switch (status) {
    case PsRequestPayloadStatusV1::ok:
      return "ok";
    case PsRequestPayloadStatusV1::invalid_argument:
      return "invalid_argument";
    case PsRequestPayloadStatusV1::resource_limit_exceeded:
      return "resource_limit_exceeded";
    case PsRequestPayloadStatusV1::frame_payload_invalid:
      return "frame_payload_invalid";
    case PsRequestPayloadStatusV1::request_semantics_invalid:
      return "request_semantics_invalid";
    case PsRequestPayloadStatusV1::request_authority_mismatch:
      return "request_authority_mismatch";
  }
  return "unknown";
}

}  // namespace scratchbird::parser::ipc
