// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "typed_result_payload_codec.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace scratchbird::parser::ipc {
namespace {

constexpr const char* kFrameInvalid =
    "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID";
constexpr const char* kResourceLimit =
    "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED";
constexpr std::uint64_t kMaximumBlobBytes =
    wire::kTypedResultCarrierMaximumBytes;
constexpr std::size_t kFieldHeaderBytes = 6;

PsTypedResultPayloadDiagnosticV1 Ok() {
  PsTypedResultPayloadDiagnosticV1 result;
  result.status = PsTypedResultPayloadStatusV1::ok;
  return result;
}

PsTypedResultPayloadDiagnosticV1 Error(
    PsTypedResultPayloadStatusV1 status,
    std::string diagnostic_code,
    std::uint16_t field_id,
    std::string detail) {
  PsTypedResultPayloadDiagnosticV1 result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic_code);
  result.field_id = field_id;
  result.detail = std::move(detail);
  return result;
}

PsTypedResultPayloadDiagnosticV1 FrameError(std::uint16_t field_id,
                                             std::string detail) {
  return Error(PsTypedResultPayloadStatusV1::frame_payload_invalid,
               kFrameInvalid, field_id, std::move(detail));
}

PsTypedResultPayloadDiagnosticV1 ResourceError(std::uint16_t field_id,
                                                std::string detail) {
  return Error(PsTypedResultPayloadStatusV1::resource_limit_exceeded,
               kResourceLimit, field_id, std::move(detail));
}

PsTypedResultPayloadDiagnosticV1 CarrierError(
    const wire::TypedResultCarrierValidationResult& validation) {
  const auto status =
      validation.status == wire::TypedResultCodecStatus::resource_limit_exceeded
          ? PsTypedResultPayloadStatusV1::resource_limit_exceeded
          : PsTypedResultPayloadStatusV1::typed_result_invalid;
  return Error(status,
               validation.diagnostic_code.empty() ? kFrameInvalid
                                                  : validation.diagnostic_code,
               0, validation.detail);
}

void AppendU16(std::vector<byte>* out, std::uint16_t value) {
  out->push_back(static_cast<byte>(value & 0xffu));
  out->push_back(static_cast<byte>((value >> 8u) & 0xffu));
}

void AppendU32(std::vector<byte>* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<byte>((value >> shift) & 0xffu));
  }
}

std::array<byte, 8> U64Bytes(std::uint64_t value) {
  std::array<byte, 8> out{};
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out[shift / 8] = static_cast<byte>((value >> shift) & 0xffu);
  }
  return out;
}

std::array<byte, 2> U16Bytes(std::uint16_t value) {
  return {static_cast<byte>(value & 0xffu),
          static_cast<byte>((value >> 8u) & 0xffu)};
}

bool AddWithinAssembledLimit(std::size_t current,
                             std::size_t increment,
                             std::size_t* next) {
  if (next == nullptr ||
      increment > std::numeric_limits<std::size_t>::max() - current) {
    return false;
  }
  *next = current + increment;
  return *next <= kPsTypedResultMaximumAssembledPayloadBytes;
}

bool AppendField(std::vector<byte>* out,
                 std::uint16_t field_id,
                 std::span<const byte> value) {
  if (out == nullptr || field_id == 0 ||
      value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  std::size_t next = 0;
  if (!AddWithinAssembledLimit(out->size(), kFieldHeaderBytes, &next) ||
      !AddWithinAssembledLimit(next, value.size(), &next)) {
    return false;
  }
  AppendU16(out, field_id);
  AppendU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  return true;
}

template <std::size_t N>
bool AppendField(std::vector<byte>* out,
                 std::uint16_t field_id,
                 const std::array<byte, N>& value) {
  return AppendField(out, field_id,
                     std::span<const byte>(value.data(), value.size()));
}

bool AppendField(std::vector<byte>* out,
                 std::uint16_t field_id,
                 const std::vector<byte>& value) {
  return AppendField(out, field_id,
                     std::span<const byte>(value.data(), value.size()));
}

std::uint16_t LoadU16(const byte* data) {
  return static_cast<std::uint16_t>(data[0]) |
         (static_cast<std::uint16_t>(data[1]) << 8u);
}

std::uint32_t LoadU32(const byte* data) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(data[shift / 8]) << shift;
  }
  return value;
}

std::uint64_t LoadU64(const byte* data) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
  }
  return value;
}

class FieldReader {
 public:
  explicit FieldReader(std::span<const byte> payload)
      : payload_(payload), offset_(2) {}

  bool Read(std::uint16_t expected_id,
            std::span<const byte>* value,
            PsTypedResultPayloadDiagnosticV1* error) {
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
  std::size_t offset_ = 0;
};

bool PrimitiveSize(std::span<const byte> value,
                   std::size_t expected,
                   std::uint16_t field_id,
                   PsTypedResultPayloadDiagnosticV1* error) {
  if (value.size() == expected) return true;
  if (error != nullptr) {
    *error = FrameError(field_id, "primitive_field_length_invalid");
  }
  return false;
}

bool BlobSize(std::span<const byte> value,
              std::uint16_t field_id,
              PsTypedResultPayloadDiagnosticV1* error) {
  if (value.size() <= kMaximumBlobBytes) return true;
  if (error != nullptr) {
    *error = ResourceError(field_id, "blob_exceeds_core_field_limit");
  }
  return false;
}

bool BeginDecode(std::span<const byte> payload,
                 PsTypedResultPayloadDiagnosticV1* error) {
  if (error == nullptr) return false;
  if (payload.size() > kPsTypedResultMaximumAssembledPayloadBytes) {
    *error = ResourceError(0, "payload_exceeds_core_assembled_limit");
    return false;
  }
  if (payload.size() < 2) {
    *error = FrameError(0, "layout_revision_missing");
    return false;
  }
  if (LoadU16(payload.data()) != kPsTypedResultPayloadLayoutRevisionV1) {
    *error = FrameError(0, "layout_revision_invalid");
    return false;
  }
  return true;
}

PsTypedResultPayloadDiagnosticV1 EncodeExecuteUnchecked(
    const wire::TypedResultExecuteCarrierV1& carrier,
    std::vector<byte>* payload) {
  if (payload == nullptr) {
    return Error(PsTypedResultPayloadStatusV1::invalid_argument,
                 kFrameInvalid, 0, "execute_output_is_null");
  }
  const std::array<byte, 1> outcome{
      static_cast<byte>(carrier.outcome)};
  const auto local_transaction_id = U64Bytes(carrier.local_transaction_id);
  const auto row_count = U64Bytes(carrier.row_count);
  const auto cursor_descriptor_version =
      U16Bytes(carrier.cursor_stream_descriptor.descriptor_version);
  const auto cursor_descriptor_generation =
      U64Bytes(carrier.cursor_stream_descriptor.descriptor_generation);
  const auto cursor_maximum_rows =
      U64Bytes(carrier.cursor_stream_descriptor.maximum_chunk_rows);
  const auto cursor_maximum_bytes =
      U64Bytes(carrier.cursor_stream_descriptor.maximum_chunk_bytes);

  std::vector<byte> encoded;
  encoded.reserve(2 + (19 * kFieldHeaderBytes) + 162 +
                  carrier.result_descriptor_vector.size() +
                  carrier.row_data_packet.size() + carrier.finality_token.size() +
                  carrier.message_vector_set.size());
  AppendU16(&encoded, kPsTypedResultPayloadLayoutRevisionV1);
  const bool appended =
      AppendField(&encoded, 1, outcome) &&
      AppendField(&encoded, 2, carrier.server_request_uuid) &&
      AppendField(&encoded, 3, carrier.transaction_uuid) &&
      AppendField(&encoded, 4, local_transaction_id) &&
      AppendField(&encoded, 5, carrier.cursor_uuid) &&
      AppendField(&encoded, 6, row_count) &&
      AppendField(&encoded, 7, carrier.result_descriptor_vector) &&
      AppendField(&encoded, 8, carrier.row_data_packet) &&
      AppendField(&encoded, 9, carrier.finality_token) &&
      AppendField(&encoded, 10, carrier.message_vector_set) &&
      AppendField(&encoded, 11,
                  carrier.cursor_stream_descriptor.descriptor_uuid) &&
      AppendField(&encoded, 12, cursor_descriptor_version) &&
      AppendField(&encoded, 13, cursor_descriptor_generation) &&
      AppendField(&encoded, 14, cursor_maximum_rows) &&
      AppendField(&encoded, 15, cursor_maximum_bytes) &&
      AppendField(&encoded, 16, carrier.query_handle.execution_uuid) &&
      AppendField(&encoded, 17, carrier.query_handle.result_set_uuid) &&
      AppendField(&encoded, 18, carrier.query_handle.row_descriptor_uuid) &&
      AppendField(&encoded, 19, carrier.query_handle.snapshot_uuid);
  if (!appended) {
    return ResourceError(0, "execute_payload_exceeds_core_assembled_limit");
  }
  *payload = std::move(encoded);
  return Ok();
}

PsTypedResultPayloadDiagnosticV1 EncodeFetchUnchecked(
    const wire::TypedResultFetchCarrierV1& carrier,
    std::vector<byte>* payload) {
  if (payload == nullptr) {
    return Error(PsTypedResultPayloadStatusV1::invalid_argument,
                 kFrameInvalid, 0, "fetch_output_is_null");
  }
  const auto row_count = U64Bytes(carrier.row_count);
  const std::array<byte, 1> end_of_cursor{
      static_cast<byte>(carrier.end_of_cursor ? 1 : 0)};
  std::vector<byte> encoded;
  encoded.reserve(2 + (5 * kFieldHeaderBytes) + 25 +
                  carrier.row_data_packet.size() +
                  carrier.message_vector_set.size());
  AppendU16(&encoded, kPsTypedResultPayloadLayoutRevisionV1);
  const bool appended =
      AppendField(&encoded, 1, carrier.cursor_uuid) &&
      AppendField(&encoded, 2, row_count) &&
      AppendField(&encoded, 3, carrier.row_data_packet) &&
      AppendField(&encoded, 4, end_of_cursor) &&
      AppendField(&encoded, 5, carrier.message_vector_set);
  if (!appended) {
    return ResourceError(0, "fetch_payload_exceeds_core_assembled_limit");
  }
  *payload = std::move(encoded);
  return Ok();
}

bool DecodeExecuteFields(std::span<const byte> payload,
                         wire::TypedResultExecuteCarrierV1* carrier,
                         PsTypedResultPayloadDiagnosticV1* error) {
  if (carrier == nullptr || !BeginDecode(payload, error)) return false;
  FieldReader reader(payload);
  std::array<std::span<const byte>, 19> fields{};
  for (std::uint16_t field_id = 1; field_id <= fields.size(); ++field_id) {
    if (!reader.Read(field_id, &fields[field_id - 1], error)) return false;
  }
  if (!reader.complete()) {
    *error = FrameError(0, "trailing_or_unlisted_field_bytes");
    return false;
  }
  const std::array<std::size_t, 19> primitive_sizes{
      1, 16, 16, 8, 16, 8, 0, 0, 0, 0, 16, 2, 8, 8, 8, 16, 16, 16, 16};
  for (std::size_t index = 0; index < primitive_sizes.size(); ++index) {
    if (primitive_sizes[index] != 0 &&
        !PrimitiveSize(fields[index], primitive_sizes[index],
                       static_cast<std::uint16_t>(index + 1), error)) {
      return false;
    }
  }
  for (const std::uint16_t field_id : {7, 8, 9, 10}) {
    if (!BlobSize(fields[field_id - 1], field_id, error)) return false;
  }

  wire::TypedResultExecuteCarrierV1 decoded;
  decoded.outcome = static_cast<wire::TypedResultExecuteOutcome>(fields[0][0]);
  std::copy(fields[1].begin(), fields[1].end(),
            decoded.server_request_uuid.begin());
  std::copy(fields[2].begin(), fields[2].end(),
            decoded.transaction_uuid.begin());
  decoded.local_transaction_id = LoadU64(fields[3].data());
  std::copy(fields[4].begin(), fields[4].end(), decoded.cursor_uuid.begin());
  decoded.row_count = LoadU64(fields[5].data());
  decoded.result_descriptor_vector.assign(fields[6].begin(), fields[6].end());
  decoded.row_data_packet.assign(fields[7].begin(), fields[7].end());
  decoded.finality_token.assign(fields[8].begin(), fields[8].end());
  decoded.message_vector_set.assign(fields[9].begin(), fields[9].end());
  std::copy(fields[10].begin(), fields[10].end(),
            decoded.cursor_stream_descriptor.descriptor_uuid.begin());
  decoded.cursor_stream_descriptor.descriptor_version =
      LoadU16(fields[11].data());
  decoded.cursor_stream_descriptor.descriptor_generation =
      LoadU64(fields[12].data());
  decoded.cursor_stream_descriptor.maximum_chunk_rows =
      LoadU64(fields[13].data());
  decoded.cursor_stream_descriptor.maximum_chunk_bytes =
      LoadU64(fields[14].data());
  std::copy(fields[15].begin(), fields[15].end(),
            decoded.query_handle.execution_uuid.begin());
  std::copy(fields[16].begin(), fields[16].end(),
            decoded.query_handle.result_set_uuid.begin());
  std::copy(fields[17].begin(), fields[17].end(),
            decoded.query_handle.row_descriptor_uuid.begin());
  std::copy(fields[18].begin(), fields[18].end(),
            decoded.query_handle.snapshot_uuid.begin());
  *carrier = std::move(decoded);
  return true;
}

bool DecodeFetchFields(std::span<const byte> payload,
                       wire::TypedResultFetchCarrierV1* carrier,
                       PsTypedResultPayloadDiagnosticV1* error) {
  if (carrier == nullptr || !BeginDecode(payload, error)) return false;
  FieldReader reader(payload);
  std::array<std::span<const byte>, 5> fields{};
  for (std::uint16_t field_id = 1; field_id <= fields.size(); ++field_id) {
    if (!reader.Read(field_id, &fields[field_id - 1], error)) return false;
  }
  if (!reader.complete()) {
    *error = FrameError(0, "trailing_or_unlisted_field_bytes");
    return false;
  }
  if (!PrimitiveSize(fields[0], 16, 1, error) ||
      !PrimitiveSize(fields[1], 8, 2, error) ||
      !BlobSize(fields[2], 3, error) ||
      !PrimitiveSize(fields[3], 1, 4, error) ||
      fields[3][0] > 1 || !BlobSize(fields[4], 5, error)) {
    if (fields[3].size() == 1 && fields[3][0] > 1) {
      *error = FrameError(4, "boolean_field_value_invalid");
    }
    return false;
  }

  wire::TypedResultFetchCarrierV1 decoded;
  std::copy(fields[0].begin(), fields[0].end(), decoded.cursor_uuid.begin());
  decoded.row_count = LoadU64(fields[1].data());
  decoded.row_data_packet.assign(fields[2].begin(), fields[2].end());
  decoded.end_of_cursor = fields[3][0] != 0;
  decoded.message_vector_set.assign(fields[4].begin(), fields[4].end());
  *carrier = std::move(decoded);
  return true;
}

template <typename Result>
void Refuse(Result* result, PsTypedResultPayloadDiagnosticV1 outcome) {
  if (result == nullptr) return;
  result->outcome = std::move(outcome);
  result->canonical_payload.clear();
  result->carrier = {};
  result->validated = {};
}

}  // namespace

PsExecuteResultPayloadCodecResultV1 EncodeAndValidatePsExecuteResultV1Payload(
    const wire::TypedResultExecuteRequestAuthorityV1& request_authority,
    const wire::TypedResultExecuteCarrierV1& carrier,
    const wire::TypedResultDescriptorAuthorityValidator& descriptor_authority) {
  PsExecuteResultPayloadCodecResultV1 result;
  const auto validated = wire::ValidateTypedResultExecuteCarrierV1(
      request_authority, carrier, descriptor_authority);
  if (!validated.ok()) {
    Refuse(&result, CarrierError(validated));
    return result;
  }
  std::vector<byte> encoded;
  const auto encoded_outcome = EncodeExecuteUnchecked(carrier, &encoded);
  if (!encoded_outcome.ok()) {
    Refuse(&result, encoded_outcome);
    return result;
  }
  result.outcome = Ok();
  result.canonical_payload = std::move(encoded);
  result.carrier = carrier;
  result.validated = validated;
  return result;
}

PsExecuteResultPayloadCodecResultV1 DecodeAndValidatePsExecuteResultV1Payload(
    std::span<const byte> payload,
    const wire::TypedResultExecuteRequestAuthorityV1& request_authority,
    const wire::TypedResultDescriptorAuthorityValidator& descriptor_authority) {
  PsExecuteResultPayloadCodecResultV1 result;
  wire::TypedResultExecuteCarrierV1 carrier;
  PsTypedResultPayloadDiagnosticV1 decode_outcome;
  if (!DecodeExecuteFields(payload, &carrier, &decode_outcome)) {
    Refuse(&result, std::move(decode_outcome));
    return result;
  }
  const auto validated = wire::ValidateTypedResultExecuteCarrierV1(
      request_authority, carrier, descriptor_authority);
  if (!validated.ok()) {
    Refuse(&result, CarrierError(validated));
    return result;
  }
  std::vector<byte> canonical;
  const auto encoded_outcome = EncodeExecuteUnchecked(carrier, &canonical);
  if (!encoded_outcome.ok()) {
    Refuse(&result, encoded_outcome);
    return result;
  }
  if (canonical.size() != payload.size() ||
      !std::equal(canonical.begin(), canonical.end(), payload.begin())) {
    Refuse(&result, FrameError(0, "payload_is_not_canonical_exact_bytes"));
    return result;
  }
  result.outcome = Ok();
  result.canonical_payload = std::move(canonical);
  result.carrier = std::move(carrier);
  result.validated = validated;
  return result;
}

PsFetchResultPayloadCodecResultV1 EncodeAndValidatePsFetchResultV1Payload(
    const wire::TypedResultFetchRequestAuthorityV1& request_authority,
    const wire::TypedResultFetchCarrierV1& carrier,
    const wire::TypedResultCursorCarrierStateV1& prior_cursor_state) {
  PsFetchResultPayloadCodecResultV1 result;
  const auto validated = wire::ValidateTypedResultFetchCarrierV1(
      request_authority, carrier, prior_cursor_state);
  if (!validated.ok()) {
    Refuse(&result, CarrierError(validated));
    return result;
  }
  std::vector<byte> encoded;
  const auto encoded_outcome = EncodeFetchUnchecked(carrier, &encoded);
  if (!encoded_outcome.ok()) {
    Refuse(&result, encoded_outcome);
    return result;
  }
  result.outcome = Ok();
  result.canonical_payload = std::move(encoded);
  result.carrier = carrier;
  result.validated = validated;
  return result;
}

PsFetchResultPayloadCodecResultV1 DecodeAndValidatePsFetchResultV1Payload(
    std::span<const byte> payload,
    const wire::TypedResultFetchRequestAuthorityV1& request_authority,
    const wire::TypedResultCursorCarrierStateV1& prior_cursor_state) {
  PsFetchResultPayloadCodecResultV1 result;
  wire::TypedResultFetchCarrierV1 carrier;
  PsTypedResultPayloadDiagnosticV1 decode_outcome;
  if (!DecodeFetchFields(payload, &carrier, &decode_outcome)) {
    Refuse(&result, std::move(decode_outcome));
    return result;
  }
  const auto validated = wire::ValidateTypedResultFetchCarrierV1(
      request_authority, carrier, prior_cursor_state);
  if (!validated.ok()) {
    Refuse(&result, CarrierError(validated));
    return result;
  }
  std::vector<byte> canonical;
  const auto encoded_outcome = EncodeFetchUnchecked(carrier, &canonical);
  if (!encoded_outcome.ok()) {
    Refuse(&result, encoded_outcome);
    return result;
  }
  if (canonical.size() != payload.size() ||
      !std::equal(canonical.begin(), canonical.end(), payload.begin())) {
    Refuse(&result, FrameError(0, "payload_is_not_canonical_exact_bytes"));
    return result;
  }
  result.outcome = Ok();
  result.canonical_payload = std::move(canonical);
  result.carrier = std::move(carrier);
  result.validated = validated;
  return result;
}

const char* PsTypedResultPayloadStatusNameV1(
    PsTypedResultPayloadStatusV1 status) {
  switch (status) {
    case PsTypedResultPayloadStatusV1::ok:
      return "ok";
    case PsTypedResultPayloadStatusV1::invalid_argument:
      return "invalid_argument";
    case PsTypedResultPayloadStatusV1::resource_limit_exceeded:
      return "resource_limit_exceeded";
    case PsTypedResultPayloadStatusV1::frame_payload_invalid:
      return "frame_payload_invalid";
    case PsTypedResultPayloadStatusV1::typed_result_invalid:
      return "typed_result_invalid";
  }
  return "unknown";
}

}  // namespace scratchbird::parser::ipc
