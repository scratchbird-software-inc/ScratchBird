// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "typed_result_transport_codec.hpp"
#include "typed_result_resource_buffer.hpp"
#include "typed_result_packet_view.hpp"

#include "datatype_binary.hpp"
#include "datatype_binary_view.hpp"
#include "canonical_utf8.hpp"
#include "sbl_numeric.hpp"
#include "datatype_layout.hpp"
#include "hash_digest.hpp"
#include "hash_digest_parts.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

namespace scratchbird::wire {
namespace {

namespace datatypes = scratchbird::core::datatypes;
namespace core_hash = scratchbird::core::hash;

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;

constexpr std::array<byte, 8> kDescriptorMagic{
    {'S', 'B', 'T', 'R', 'D', 'S', '0', '1'}};
constexpr std::array<byte, 8> kBatchMagic{
    {'S', 'B', 'T', 'R', 'B', 'T', '0', '1'}};
constexpr std::string_view kDescriptorEvidenceDomain =
    "ScratchBird.PsResultDescriptorVector.V1";
constexpr std::string_view kBatchEvidenceDomain =
    "ScratchBird.PsRowDataPacket.V1";

constexpr std::size_t kDescriptorEvidenceOffset = 96;
constexpr std::size_t kBatchEvidenceOffset = 192;
constexpr u32 kColumnDescriptorFixedBytes = kTypedResultColumnDescriptorPrefixBytes;
constexpr u32 kRowFrameFixedBytes = 16;
constexpr u32 kCellFrameFixedBytes = 20;
constexpr std::uint8_t kExplicitNullStateEncoding = 1;
constexpr std::uint8_t kDatatypeBinaryValueEncoding = 1;
constexpr u64 kMaxTransportFrameBytes = 16ull * 1024ull * 1024ull;
constexpr u32 kMaxColumnCount = 16384;
constexpr u32 kMaxRowCount = 1048576;
constexpr u32 kMaxColumnNameBytes = 4096;
constexpr u32 kMaxCodecIdBytes = 255;

constexpr const char* kFrameInvalid =
    "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID";
constexpr const char* kResourceLimitExceeded =
    "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED";
constexpr const char* kDatatypeDescriptorInvalid =
    "DATATYPE.DESCRIPTOR.INVALID";
constexpr const char* kConnectionMismatch =
    "PARSER_SERVER_IPC.CONNECTION_MISMATCH";
constexpr const char* kSequenceInvalid =
    "PARSER_SERVER_IPC.SEQUENCE_INVALID";
constexpr const char* kSystemUuidInvalid = "UUID.ENGINE_IDENTITY_NOT_V7";

enum class DescriptorValidationKind {
  ok,
  frame,
  datatype,
  resource,
  system_uuid,
};

struct DescriptorValidation {
  DescriptorValidationKind kind = DescriptorValidationKind::ok;
  std::string detail;

  bool ok() const { return kind == DescriptorValidationKind::ok; }
};

TypedResultDescriptorCodecResult DescriptorError(
    TypedResultCodecStatus status,
    std::string diagnostic_code,
    std::string detail) {
  TypedResultDescriptorCodecResult result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic_code);
  result.detail = std::move(detail);
  return result;
}

TypedResultBatchCodecResult BatchError(TypedResultCodecStatus status,
                                       std::string diagnostic_code,
                                       std::string detail) {
  TypedResultBatchCodecResult result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic_code);
  result.detail = std::move(detail);
  return result;
}

bool UuidPresent(const TypedResultUuid& uuid) {
  return std::any_of(uuid.begin(), uuid.end(), [](byte value) {
    return value != 0;
  });
}

bool InvalidPresentSystemUuid(const TypedResultUuid& uuid) {
  scratchbird::core::uuid::Uuid identity;
  identity.bytes = uuid;
  return !identity.is_nil() &&
         !scratchbird::core::uuid::IsEngineIdentityUuid(identity);
}

bool BatchSystemIdentitiesValid(const TypedResultBatch& batch,
                                const TypedResultCarrierBinding& binding) {
  for (const auto* identity : {&batch.execution_uuid, &batch.result_set_uuid,
       &batch.batch_uuid, &batch.row_descriptor_uuid, &batch.snapshot_uuid,
       &batch.cursor_uuid, &binding.execution_uuid, &binding.result_set_uuid,
       &binding.snapshot_uuid, &binding.cursor_uuid,
       &binding.cursor_stream_descriptor_uuid}) {
    if (InvalidPresentSystemUuid(*identity)) return false;
  }
  return true;
}

bool HashPresent(const TypedResultEvidenceHash& hash) {
  return std::any_of(hash.begin(), hash.end(), [](byte value) {
    return value != 0;
  });
}

bool SameHash(const TypedResultEvidenceHash& left,
              const TypedResultEvidenceHash& right) {
  byte difference = 0;
  for (std::size_t index = 0; index != left.size(); ++index)
    difference = static_cast<byte>(difference | (left[index] ^ right[index]));
  return difference == 0;
}

bool ValidUtf8(std::string_view value) {
  return value.find('\0') == std::string_view::npos &&
         datatypes::ValidateCanonicalUtf8(
             reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

bool ValidCodecId(std::string_view value) {
  if (value.empty() || value.size() > kMaxCodecIdBytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-' || character == ':';
  });
}

bool ValidNullability(TypedResultNullability nullability) {
  switch (nullability) {
    case TypedResultNullability::not_null:
    case TypedResultNullability::nullable:
    case TypedResultNullability::unknown:
      return true;
  }
  return false;
}

bool ValidValueState(TypedResultValueState state) {
  switch (state) {
    case TypedResultValueState::value_present:
    case TypedResultValueState::sql_null:
      return true;
  }
  return false;
}

template <typename ByteBuffer>
void AppendU16(ByteBuffer* out, u16 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

template <typename ByteBuffer>
void AppendU32(ByteBuffer* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

template <typename ByteBuffer>
void AppendU64(ByteBuffer* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle64(out->data() + offset, value);
}

void AppendUuid(std::vector<byte>* out, const TypedResultUuid& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void AppendString(std::vector<byte>* out, std::string_view value) {
  out->insert(out->end(), value.begin(), value.end());
}

template <typename ByteSequence>
bool ReadU8(const ByteSequence& bytes,
            std::size_t* offset,
            std::uint8_t* value) {
  if (*offset >= bytes.size()) {
    return false;
  }
  *value = bytes[(*offset)++];
  return true;
}

template <typename ByteSequence>
bool ReadU16(const ByteSequence& bytes,
             std::size_t* offset,
             u16* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < sizeof(*value)) {
    return false;
  }
  *value = LoadLittle16(bytes.data() + *offset);
  *offset += sizeof(*value);
  return true;
}

template <typename ByteSequence>
bool ReadU32(const ByteSequence& bytes,
             std::size_t* offset,
             u32* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < sizeof(*value)) {
    return false;
  }
  *value = LoadLittle32(bytes.data() + *offset);
  *offset += sizeof(*value);
  return true;
}

template <typename ByteSequence>
bool ReadU64(const ByteSequence& bytes,
             std::size_t* offset,
             u64* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < sizeof(*value)) {
    return false;
  }
  *value = LoadLittle64(bytes.data() + *offset);
  *offset += sizeof(*value);
  return true;
}

template <typename ByteSequence>
bool ReadUuid(const ByteSequence& bytes,
              std::size_t* offset,
              TypedResultUuid* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < value->size()) {
    return false;
  }
  std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(*offset),
              value->size(), value->begin());
  *offset += value->size();
  return true;
}

template <typename ByteSequence>
bool ReadString(const ByteSequence& bytes,
                std::size_t* offset,
                std::size_t size,
                std::string* value) {
  if (*offset > bytes.size() || size > bytes.size() - *offset) {
    return false;
  }
  value->assign(reinterpret_cast<const char*>(bytes.data() + *offset), size);
  *offset += size;
  return true;
}

bool AddWithinLimit(u64 left, u64 right, u64 limit, u64* sum) {
  if (left > limit || right > limit - left) {
    return false;
  }
  *sum = left + right;
  return true;
}

DescriptorValidation ValidateDescriptor(
    const TypedResultRowDescriptor& descriptor) {
  if (InvalidPresentSystemUuid(descriptor.descriptor_uuid) ||
      InvalidPresentSystemUuid(descriptor.datatype_catalog_snapshot_uuid))
    return {DescriptorValidationKind::system_uuid, "row_descriptor_system_uuid_invalid"};
  if (!UuidPresent(descriptor.descriptor_uuid) ||
      descriptor.descriptor_generation == 0) {
    return {DescriptorValidationKind::frame,
            "row_descriptor_identity_required"};
  }
  if (!UuidPresent(descriptor.datatype_catalog_snapshot_uuid) ||
      descriptor.datatype_catalog_generation == 0 ||
      descriptor.datatype_registry_generation == 0) {
    return {DescriptorValidationKind::datatype,
            "datatype_catalog_identity_required"};
  }
  if (descriptor.columns.empty() ||
      descriptor.columns.size() > kMaxColumnCount) {
    return {descriptor.columns.size() > kMaxColumnCount
                ? DescriptorValidationKind::resource
                : DescriptorValidationKind::frame,
            "column_count_invalid"};
  }

  std::map<std::string, u32> name_occurrences;
  for (std::size_t index = 0; index < descriptor.columns.size(); ++index) {
    const auto& column = descriptor.columns[index];
    if (InvalidPresentSystemUuid(column.descriptor_uuid) ||
        InvalidPresentSystemUuid(column.type_uuid))
      return {DescriptorValidationKind::system_uuid, "column_system_uuid_invalid"};
    if (column.ordinal != index) {
      return {DescriptorValidationKind::frame,
              "column_ordinal_not_contiguous"};
    }
    if (!ValidTypedResultColumnName(column.name)) {
      return {DescriptorValidationKind::frame,
              "column_name_invalid_utf8"};
    }
    const u32 expected_occurrence = name_occurrences[column.name]++;
    if (column.name_occurrence != expected_occurrence) {
      return {DescriptorValidationKind::frame,
              "column_name_occurrence_invalid"};
    }
    if (!ValidNullability(column.nullability)) {
      return {DescriptorValidationKind::frame,
              "column_nullability_invalid"};
    }
    if (!UuidPresent(column.descriptor_uuid) ||
        column.descriptor_generation == 0 || !UuidPresent(column.type_uuid) ||
        column.type_generation == 0 || !ValidCodecId(column.codec_id) ||
        column.codec_version == 0 || column.codec_generation == 0 ||
        column.canonical_type_id == CanonicalTypeId::unknown ||
        column.canonical_type_id == CanonicalTypeId::null_type) {
      return {DescriptorValidationKind::datatype,
              "column_type_codec_identity_invalid"};
    }
    const auto layout =
        datatypes::LookupDatatypeStorageLayout(column.canonical_type_id);
    if (!layout.ok()) {
      return {DescriptorValidationKind::datatype,
              "column_canonical_type_unsupported"};
    }
    const bool decimal_value = column.canonical_type_id == CanonicalTypeId::decimal;
    if (decimal_value &&
        column.canonical_value_bytes !=
            scratchbird::libraries::sbl_numeric::kExactDecimalBinaryBytes) {
      return {DescriptorValidationKind::datatype,
              "column_decimal_value_width_mismatch"};
    }
    if (!decimal_value && layout.layout.storage_class ==
            datatypes::DatatypeStorageClass::inline_fixed &&
        column.canonical_value_bytes != layout.layout.inline_bytes) {
      return {DescriptorValidationKind::datatype,
              "column_fixed_width_mismatch"};
    }
    if (!decimal_value && layout.layout.storage_class !=
            datatypes::DatatypeStorageClass::inline_fixed &&
        column.canonical_value_bytes != 0) {
      return {DescriptorValidationKind::datatype,
              "column_variable_width_must_be_zero"};
    }
  }
  return {};
}

TypedResultEvidenceHash DigestHash(
    const core_hash::HashDigestResult& digest) {
  TypedResultEvidenceHash hash{};
  std::copy(digest.digest.begin(), digest.digest.end(), hash.begin());
  return hash;
}

template <typename ByteSequence>
core_hash::HashDigestResult ComputeEvidence(
    std::string_view domain,
    const ByteSequence& canonical_bytes,
    std::size_t evidence_offset) {
  if (evidence_offset > canonical_bytes.size() ||
      kTypedResultEvidenceHashBytes > canonical_bytes.size() - evidence_offset) {
    core_hash::HashDigestResult failure;
    failure.status = {core::platform::StatusCode::platform_required_feature_missing,
                      core::platform::Severity::error, core::platform::Subsystem::platform};
    failure.diagnostic = core_hash::MakeHashDigestDiagnostic(
        failure.status, "SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed",
        "evidence_slot_outside_frame");
    return failure;
  }
  const std::array<byte, kTypedResultEvidenceHashBytes> zero{};
  const auto after_evidence = evidence_offset + zero.size();
  const core_hash::HashDigestSegment parts[] = {
      {reinterpret_cast<const byte*>(domain.data()), domain.size()},
      {canonical_bytes.data(), evidence_offset},
      {zero.data(), zero.size()},
      {canonical_bytes.data() + after_evidence, canonical_bytes.size() - after_evidence}};
  return core_hash::ComputeSha256DigestParts(parts, 4);
}

bool CellMatchesColumn(const TypedResultCell& cell,
                       const TypedResultColumnDescriptor& column) {
  return cell.column_ordinal == column.ordinal &&
         cell.name_occurrence == column.name_occurrence;
}

TypedResultBatchCodecResult DescriptorForBatch(
    const TypedResultRowDescriptor& descriptor,
    TypedResultRowDescriptor* canonical_descriptor) {
  auto encoded = EncodeTypedResultRowDescriptor(descriptor);
  if (!encoded.ok()) {
    return BatchError(encoded.status, encoded.diagnostic_code,
                      "batch_descriptor:" + encoded.detail);
  }
  *canonical_descriptor = std::move(encoded.descriptor);
  return {};
}

TypedResultBatchCodecResult ValidateCarrierBinding(
    const TypedResultBatch& batch,
    const TypedResultCarrierBinding& carrier_binding,
    u64 row_count) {
  if (carrier_binding.row_count != row_count ||
      carrier_binding.end_of_rowset != batch.end_of_rowset) {
    return BatchError(TypedResultCodecStatus::shape_invalid, kFrameInvalid,
                      "outer_row_count_or_end_state_mismatch");
  }
  if (!UuidPresent(carrier_binding.execution_uuid) ||
      !UuidPresent(carrier_binding.result_set_uuid) ||
      !UuidPresent(carrier_binding.snapshot_uuid) ||
      batch.execution_uuid != carrier_binding.execution_uuid ||
      batch.result_set_uuid != carrier_binding.result_set_uuid ||
      batch.snapshot_uuid != carrier_binding.snapshot_uuid) {
    return BatchError(TypedResultCodecStatus::cursor_mismatch,
                      kConnectionMismatch,
                      "query_result_handle_binding_mismatch");
  }
  const bool cursor_present = UuidPresent(batch.cursor_uuid);
  if (batch.cursor_bound != cursor_present) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid,
                      "cursor_bound_flag_and_identity_mismatch");
  }
  switch (carrier_binding.kind) {
    case TypedResultCarrierKind::ps_execute_result_v1:
      if (batch.cursor_bound || UuidPresent(carrier_binding.cursor_uuid) ||
          UuidPresent(carrier_binding.cursor_stream_descriptor_uuid) ||
          carrier_binding.cursor_stream_descriptor_version != 0 ||
          carrier_binding.cursor_stream_descriptor_generation != 0 ||
          batch.batch_ordinal != 0 || !batch.end_of_rowset) {
        return BatchError(TypedResultCodecStatus::shape_invalid,
                          kFrameInvalid,
                          "execute_result_batch_binding_invalid");
      }
      break;
    case TypedResultCarrierKind::ps_fetch_result_v1:
      if (!batch.cursor_bound ||
          !UuidPresent(carrier_binding.cursor_uuid) ||
          batch.cursor_uuid != carrier_binding.cursor_uuid ||
          !UuidPresent(carrier_binding.cursor_stream_descriptor_uuid) ||
          carrier_binding.cursor_stream_descriptor_version != 1 ||
          carrier_binding.cursor_stream_descriptor_generation == 0) {
        return BatchError(TypedResultCodecStatus::cursor_mismatch,
                          kConnectionMismatch,
                          "fetch_result_cursor_binding_mismatch");
      }
      break;
    case TypedResultCarrierKind::public_engine_abi:
      if (batch.cursor_uuid != carrier_binding.cursor_uuid ||
          (batch.cursor_bound !=
           UuidPresent(carrier_binding.cursor_stream_descriptor_uuid)) ||
          (batch.cursor_bound !=
           (carrier_binding.cursor_stream_descriptor_version == 1)) ||
          (!batch.cursor_bound &&
           carrier_binding.cursor_stream_descriptor_version != 0) ||
          (batch.cursor_bound !=
           (carrier_binding.cursor_stream_descriptor_generation != 0))) {
        return BatchError(TypedResultCodecStatus::cursor_mismatch,
                          kConnectionMismatch,
                          "public_result_cursor_binding_mismatch");
      }
      break;
    default:
      return BatchError(TypedResultCodecStatus::invalid_argument,
                        kFrameInvalid, "carrier_kind_invalid");
  }
  return {};
}

struct BorrowedByteSequence {
  const byte* first;
  std::size_t length;
  const byte* data() const noexcept { return first; }
  const byte* begin() const noexcept { return first; }
  std::size_t size() const noexcept { return length; }
  byte operator[](std::size_t offset) const noexcept { return first[offset]; }
};

TypedResultPacketViewResult PacketError(TypedResultCodecStatus status,
                                        std::string diagnostic, std::string detail) {
  TypedResultPacketViewResult result;
  result.status = status;
  result.diagnostic_code = std::move(diagnostic);
  result.detail = std::move(detail);
  return result;
}

void WriteRowHeader(u32 row_bytes, u32 cell_count, u64 ordinal, byte* destination) noexcept {
  StoreLittle32(destination, row_bytes);
  StoreLittle32(destination + 4, cell_count);
  StoreLittle64(destination + 8, ordinal);
}

void WriteCellHeader(const TypedResultCell& cell, u32 value_bytes, byte* destination) noexcept {
  StoreLittle32(destination, kCellFrameFixedBytes + value_bytes);
  StoreLittle32(destination + 4, cell.column_ordinal);
  StoreLittle32(destination + 8, cell.name_occurrence);
  destination[12] = static_cast<byte>(cell.state);
  destination[13] = kDatatypeBinaryValueEncoding;
  StoreLittle16(destination + 14, 0);
  StoreLittle32(destination + 16, value_bytes);
}

void WriteBatchHeader(const TypedResultBatch& batch,
                      const TypedResultRowDescriptor& canonical_descriptor,
                      u32 row_count, u64 rows_bytes, byte* destination) noexcept {
  std::memset(destination, 0, kTypedResultBatchHeaderBytes);
  std::copy(kBatchMagic.begin(), kBatchMagic.end(), destination);
  StoreLittle16(destination + 8, kTypedResultTransportVersion);
  StoreLittle16(destination + 10, kTypedResultBatchHeaderBytes);
  const u32 flags = (batch.end_of_rowset ? 1u : 0u) |
                    (batch.cursor_bound ? 2u : 0u);
  StoreLittle32(destination + 12, flags);
  StoreLittle64(destination + 16, kTypedResultBatchHeaderBytes + rows_bytes);
  std::copy(batch.execution_uuid.begin(), batch.execution_uuid.end(),
            destination + 24);
  std::copy(batch.result_set_uuid.begin(), batch.result_set_uuid.end(),
            destination + 40);
  std::copy(batch.batch_uuid.begin(), batch.batch_uuid.end(),
            destination + 56);
  StoreLittle64(destination + 72, batch.batch_ordinal);
  std::copy(canonical_descriptor.descriptor_uuid.begin(),
            canonical_descriptor.descriptor_uuid.end(),
            destination + 80);
  StoreLittle64(destination + 96,
                canonical_descriptor.descriptor_generation);
  std::copy(canonical_descriptor.descriptor_evidence_sha256.begin(),
            canonical_descriptor.descriptor_evidence_sha256.end(),
            destination + 104);
  StoreLittle32(destination + 136,
                row_count);
  StoreLittle32(destination + 140,
                static_cast<u32>(canonical_descriptor.columns.size()));
  StoreLittle64(destination + 144,
                rows_bytes);
  std::copy(batch.snapshot_uuid.begin(), batch.snapshot_uuid.end(),
            destination + 152);
  std::copy(batch.cursor_uuid.begin(), batch.cursor_uuid.end(),
            destination + 168);
}

TypedResultPacketHeader PacketHeaderFromBatch(const TypedResultBatch& batch, u32 rows, u32 columns) noexcept {
  return {batch.execution_uuid, batch.result_set_uuid, batch.batch_uuid,
          batch.batch_ordinal, batch.end_of_rowset, batch.cursor_bound,
          batch.row_descriptor_uuid, batch.row_descriptor_generation,
          batch.descriptor_evidence_sha256, batch.snapshot_uuid, batch.cursor_uuid,
          batch.batch_evidence_sha256, rows, columns};
}

TypedResultBatch BatchFromPacketHeader(const TypedResultPacketHeader& header) {
  TypedResultBatch batch;
  batch.execution_uuid = header.execution_uuid;
  batch.result_set_uuid = header.result_set_uuid;
  batch.batch_uuid = header.batch_uuid;
  batch.batch_ordinal = header.batch_ordinal;
  batch.end_of_rowset = header.end_of_rowset;
  batch.cursor_bound = header.cursor_bound;
  batch.row_descriptor_uuid = header.row_descriptor_uuid;
  batch.row_descriptor_generation = header.row_descriptor_generation;
  batch.descriptor_evidence_sha256 = header.descriptor_evidence_sha256;
  batch.snapshot_uuid = header.snapshot_uuid;
  batch.cursor_uuid = header.cursor_uuid;
  batch.batch_evidence_sha256 = header.batch_evidence_sha256;
  return batch;
}

}  // namespace

bool ValidTypedResultColumnName(std::string_view name) {
  return name.size() <= kMaxColumnNameBytes && ValidUtf8(name);
}

const char* TypedResultCodecStatusName(TypedResultCodecStatus status) {
  switch (status) {
    case TypedResultCodecStatus::ok:
      return "ok";
    case TypedResultCodecStatus::invalid_argument:
      return "invalid_argument";
    case TypedResultCodecStatus::malformed_frame:
      return "malformed_frame";
    case TypedResultCodecStatus::unsupported_version:
      return "unsupported_version";
    case TypedResultCodecStatus::evidence_mismatch:
      return "evidence_mismatch";
    case TypedResultCodecStatus::resource_limit_exceeded:
      return "resource_limit_exceeded";
    case TypedResultCodecStatus::descriptor_invalid:
      return "descriptor_invalid";
    case TypedResultCodecStatus::descriptor_mismatch:
      return "descriptor_mismatch";
    case TypedResultCodecStatus::cursor_mismatch:
      return "cursor_mismatch";
    case TypedResultCodecStatus::sequence_mismatch:
      return "sequence_mismatch";
    case TypedResultCodecStatus::shape_invalid:
      return "shape_invalid";
    case TypedResultCodecStatus::value_invalid:
      return "value_invalid";
  }
  return "unknown";
}

TypedResultDescriptorCodecResult EncodeTypedResultRowDescriptor(
    const TypedResultRowDescriptor& descriptor) {
  const auto validation = ValidateDescriptor(descriptor);
  if (!validation.ok()) {
    if (validation.kind == DescriptorValidationKind::system_uuid)
      return DescriptorError(TypedResultCodecStatus::descriptor_invalid,
                             kSystemUuidInvalid, validation.detail);
    if (validation.kind == DescriptorValidationKind::resource) {
      return DescriptorError(TypedResultCodecStatus::resource_limit_exceeded,
                             kResourceLimitExceeded, validation.detail);
    }
    return DescriptorError(
        validation.kind == DescriptorValidationKind::frame
            ? TypedResultCodecStatus::invalid_argument
            : TypedResultCodecStatus::descriptor_invalid,
        validation.kind == DescriptorValidationKind::frame
            ? kFrameInvalid
            : kDatatypeDescriptorInvalid,
        validation.detail);
  }

  std::vector<byte> columns;
  for (const auto& column : descriptor.columns) {
    const u64 record_bytes_u64 =
        static_cast<u64>(kColumnDescriptorFixedBytes) + column.name.size() +
        column.codec_id.size();
    if (record_bytes_u64 > std::numeric_limits<u32>::max()) {
      return DescriptorError(TypedResultCodecStatus::resource_limit_exceeded,
                             kResourceLimitExceeded,
                             "column_descriptor_too_large");
    }
    AppendU32(&columns, static_cast<u32>(record_bytes_u64));
    AppendU32(&columns, column.ordinal);
    AppendU32(&columns, column.name_occurrence);
    columns.push_back(static_cast<byte>(column.nullability));
    columns.push_back(kExplicitNullStateEncoding);
    AppendU16(&columns, 0);
    AppendUuid(&columns, column.descriptor_uuid);
    AppendU64(&columns, column.descriptor_generation);
    AppendUuid(&columns, column.type_uuid);
    AppendU64(&columns, column.type_generation);
    AppendU32(&columns, static_cast<u32>(column.canonical_type_id));
    AppendU16(&columns, column.codec_version);
    AppendU16(&columns, 0);
    AppendU64(&columns, column.codec_generation);
    AppendU32(&columns, column.canonical_value_bytes);
    AppendU32(&columns, static_cast<u32>(column.name.size()));
    AppendU32(&columns, static_cast<u32>(column.codec_id.size()));
    AppendString(&columns, column.name);
    AppendString(&columns, column.codec_id);
  }

  u64 total_bytes = 0;
  if (!AddWithinLimit(kTypedResultRowDescriptorHeaderBytes, columns.size(),
                      kMaxTransportFrameBytes, &total_bytes)) {
    return DescriptorError(TypedResultCodecStatus::resource_limit_exceeded,
                           kResourceLimitExceeded,
                           "row_descriptor_frame_too_large");
  }

  TypedResultDescriptorCodecResult result;
  result.encoded.assign(kTypedResultRowDescriptorHeaderBytes, 0);
  std::copy(kDescriptorMagic.begin(), kDescriptorMagic.end(),
            result.encoded.begin());
  StoreLittle16(result.encoded.data() + 8, kTypedResultTransportVersion);
  StoreLittle16(result.encoded.data() + 10,
                kTypedResultRowDescriptorHeaderBytes);
  StoreLittle32(result.encoded.data() + 12, 0);
  StoreLittle64(result.encoded.data() + 16, total_bytes);
  std::copy(descriptor.descriptor_uuid.begin(), descriptor.descriptor_uuid.end(),
            result.encoded.begin() + 24);
  StoreLittle64(result.encoded.data() + 40,
                descriptor.descriptor_generation);
  std::copy(descriptor.datatype_catalog_snapshot_uuid.begin(),
            descriptor.datatype_catalog_snapshot_uuid.end(),
            result.encoded.begin() + 48);
  StoreLittle64(result.encoded.data() + 64,
                descriptor.datatype_catalog_generation);
  StoreLittle64(result.encoded.data() + 72,
                descriptor.datatype_registry_generation);
  StoreLittle32(result.encoded.data() + 80,
                static_cast<u32>(descriptor.columns.size()));
  StoreLittle32(result.encoded.data() + 84, 0);
  StoreLittle64(result.encoded.data() + 88,
                static_cast<u64>(columns.size()));
  result.encoded.insert(result.encoded.end(), columns.begin(), columns.end());

  const auto digest = ComputeEvidence(kDescriptorEvidenceDomain,
                                      result.encoded, kDescriptorEvidenceOffset);
  if (!digest.ok()) {
    return DescriptorError(TypedResultCodecStatus::invalid_argument,
                           kFrameInvalid,
                           digest.diagnostic.diagnostic_code);
  }
  const auto evidence = DigestHash(digest);
  if (HashPresent(descriptor.descriptor_evidence_sha256) &&
      !SameHash(descriptor.descriptor_evidence_sha256, evidence)) {
    return DescriptorError(TypedResultCodecStatus::evidence_mismatch,
                           kFrameInvalid,
                           "provided_descriptor_evidence_mismatch");
  }
  std::copy(evidence.begin(), evidence.end(),
            result.encoded.begin() + kDescriptorEvidenceOffset);

  result.status = TypedResultCodecStatus::ok;
  result.descriptor = descriptor;
  result.descriptor.descriptor_evidence_sha256 = evidence;
  return result;
}

TypedResultDescriptorCodecResult DecodeTypedResultRowDescriptor(
    const std::vector<byte>& encoded) {
  if (encoded.size() < kTypedResultRowDescriptorHeaderBytes) {
    return DescriptorError(TypedResultCodecStatus::malformed_frame,
                           kFrameInvalid, "row_descriptor_size_invalid");
  }
  if (encoded.size() > kMaxTransportFrameBytes) {
    return DescriptorError(TypedResultCodecStatus::resource_limit_exceeded,
                           kResourceLimitExceeded,
                           "row_descriptor_size_limit_exceeded");
  }
  if (!std::equal(kDescriptorMagic.begin(), kDescriptorMagic.end(),
                  encoded.begin())) {
    return DescriptorError(TypedResultCodecStatus::malformed_frame,
                           kFrameInvalid, "row_descriptor_magic_invalid");
  }
  const u16 version = LoadLittle16(encoded.data() + 8);
  if (version != kTypedResultTransportVersion) {
    return DescriptorError(TypedResultCodecStatus::unsupported_version,
                           kFrameInvalid,
                           "row_descriptor_version_unsupported");
  }
  const u16 header_bytes = LoadLittle16(encoded.data() + 10);
  const u32 flags = LoadLittle32(encoded.data() + 12);
  const u64 total_bytes = LoadLittle64(encoded.data() + 16);
  const u32 column_count = LoadLittle32(encoded.data() + 80);
  const u32 reserved = LoadLittle32(encoded.data() + 84);
  const u64 columns_bytes = LoadLittle64(encoded.data() + 88);
  if (column_count > kMaxColumnCount) {
    return DescriptorError(TypedResultCodecStatus::resource_limit_exceeded,
                           kResourceLimitExceeded,
                           "column_count_limit_exceeded");
  }
  if (header_bytes != kTypedResultRowDescriptorHeaderBytes || flags != 0 ||
      reserved != 0 || total_bytes != encoded.size() ||
      columns_bytes != encoded.size() - header_bytes || column_count == 0) {
    return DescriptorError(TypedResultCodecStatus::malformed_frame,
                           kFrameInvalid,
                           "row_descriptor_header_invalid");
  }

  TypedResultEvidenceHash expected_evidence{};
  std::copy_n(encoded.begin() + kDescriptorEvidenceOffset,
              expected_evidence.size(), expected_evidence.begin());
  const auto digest = ComputeEvidence(kDescriptorEvidenceDomain,
                                      encoded, kDescriptorEvidenceOffset);
  if (!digest.ok()) {
    return DescriptorError(TypedResultCodecStatus::invalid_argument,
                           kFrameInvalid,
                           digest.diagnostic.diagnostic_code);
  }
  if (!SameHash(expected_evidence, DigestHash(digest))) {
    return DescriptorError(TypedResultCodecStatus::evidence_mismatch,
                           kFrameInvalid,
                           "row_descriptor_sha256_mismatch");
  }

  TypedResultRowDescriptor descriptor;
  std::size_t header_offset = 24;
  if (!ReadUuid(encoded, &header_offset, &descriptor.descriptor_uuid) ||
      !ReadU64(encoded, &header_offset, &descriptor.descriptor_generation) ||
      !ReadUuid(encoded, &header_offset,
                &descriptor.datatype_catalog_snapshot_uuid) ||
      !ReadU64(encoded, &header_offset,
               &descriptor.datatype_catalog_generation) ||
      !ReadU64(encoded, &header_offset,
               &descriptor.datatype_registry_generation)) {
    return DescriptorError(TypedResultCodecStatus::malformed_frame,
                           kFrameInvalid,
                           "row_descriptor_identity_truncated");
  }
  descriptor.descriptor_evidence_sha256 = expected_evidence;

  std::size_t offset = header_bytes;
  descriptor.columns.reserve(column_count);
  for (u32 index = 0; index < column_count; ++index) {
    const std::size_t record_begin = offset;
    u32 record_bytes = 0;
    TypedResultColumnDescriptor column;
    std::uint8_t nullability = 0;
    std::uint8_t null_encoding = 0;
    u16 record_reserved = 0;
    u16 codec_reserved = 0;
    u32 canonical_type = 0;
    u32 name_bytes = 0;
    u32 codec_bytes = 0;
    if (!ReadU32(encoded, &offset, &record_bytes) ||
        record_bytes < kColumnDescriptorFixedBytes ||
        record_begin > encoded.size() ||
        record_bytes > encoded.size() - record_begin ||
        !ReadU32(encoded, &offset, &column.ordinal) ||
        !ReadU32(encoded, &offset, &column.name_occurrence) ||
        !ReadU8(encoded, &offset, &nullability) ||
        !ReadU8(encoded, &offset, &null_encoding) ||
        !ReadU16(encoded, &offset, &record_reserved) ||
        !ReadUuid(encoded, &offset, &column.descriptor_uuid) ||
        !ReadU64(encoded, &offset, &column.descriptor_generation) ||
        !ReadUuid(encoded, &offset, &column.type_uuid) ||
        !ReadU64(encoded, &offset, &column.type_generation) ||
        !ReadU32(encoded, &offset, &canonical_type) ||
        !ReadU16(encoded, &offset, &column.codec_version) ||
        !ReadU16(encoded, &offset, &codec_reserved) ||
        !ReadU64(encoded, &offset, &column.codec_generation) ||
        !ReadU32(encoded, &offset, &column.canonical_value_bytes) ||
        !ReadU32(encoded, &offset, &name_bytes) ||
        !ReadU32(encoded, &offset, &codec_bytes) ||
        name_bytes > kMaxColumnNameBytes ||
        codec_bytes == 0 || codec_bytes > kMaxCodecIdBytes ||
        static_cast<u64>(name_bytes) + codec_bytes !=
            static_cast<u64>(record_bytes - kColumnDescriptorFixedBytes) ||
        !ReadString(encoded, &offset, name_bytes, &column.name) ||
        !ReadString(encoded, &offset, codec_bytes, &column.codec_id) ||
        offset != record_begin + record_bytes || record_reserved != 0 ||
        codec_reserved != 0 ||
        null_encoding != kExplicitNullStateEncoding) {
      return DescriptorError(TypedResultCodecStatus::malformed_frame,
                             kFrameInvalid,
                             "column_descriptor_record_invalid");
    }
    column.nullability = static_cast<TypedResultNullability>(nullability);
    column.canonical_type_id = static_cast<CanonicalTypeId>(canonical_type);
    descriptor.columns.push_back(std::move(column));
  }
  if (offset != encoded.size()) {
    return DescriptorError(TypedResultCodecStatus::malformed_frame,
                           kFrameInvalid,
                           "row_descriptor_trailing_bytes");
  }
  const auto validation = ValidateDescriptor(descriptor);
  if (!validation.ok()) {
    if (validation.kind == DescriptorValidationKind::system_uuid)
      return DescriptorError(TypedResultCodecStatus::descriptor_invalid,
                             kSystemUuidInvalid, validation.detail);
    if (validation.kind == DescriptorValidationKind::resource) {
      return DescriptorError(TypedResultCodecStatus::resource_limit_exceeded,
                             kResourceLimitExceeded, validation.detail);
    }
    return DescriptorError(
        validation.kind == DescriptorValidationKind::frame
            ? TypedResultCodecStatus::malformed_frame
            : TypedResultCodecStatus::descriptor_invalid,
        validation.kind == DescriptorValidationKind::frame
            ? kFrameInvalid
            : kDatatypeDescriptorInvalid,
        validation.detail);
  }
  const auto canonical = EncodeTypedResultRowDescriptor(descriptor);
  if (!canonical.ok() || canonical.encoded != encoded) {
    return DescriptorError(TypedResultCodecStatus::malformed_frame,
                           kFrameInvalid,
                           "row_descriptor_noncanonical_reencode");
  }

  TypedResultDescriptorCodecResult result;
  result.status = TypedResultCodecStatus::ok;
  result.encoded = encoded;
  result.descriptor = std::move(descriptor);
  return result;
}

namespace {
template <typename ByteBuffer>
TypedResultBatchCodecResult EncodeTypedResultBatchStorage(
    const TypedResultBatch& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    u64 maximum_bytes,
    ByteBuffer& output) {
  // First admit the complete byte shape without copying an untrusted payload
  // or materializing a descriptor. All arithmetic uses remaining capacity;
  // even a single oversize cell must fail before its first allocation/copy.
  const u64 packet_limit = std::min(maximum_bytes, kMaxTransportFrameBytes);
  if (batch.rows.size() > kMaxRowCount ||
      descriptor.columns.size() > kMaxColumnCount ||
      packet_limit < kTypedResultBatchHeaderBytes) {
    return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded, "batch_frame_too_large");
  }
  u64 admitted_bytes = kTypedResultBatchHeaderBytes;
  for (const auto& row : batch.rows) {
    if (row.cells.size() > kMaxColumnCount ||
        !AddWithinLimit(admitted_bytes, kRowFrameFixedBytes,
                        packet_limit, &admitted_bytes)) {
      return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                        kResourceLimitExceeded, "batch_frame_too_large");
    }
    for (const auto& cell : row.cells) {
      if (!AddWithinLimit(admitted_bytes,
                          kCellFrameFixedBytes + datatypes::kDatatypeBinaryEnvelopeHeaderBytes,
                          packet_limit, &admitted_bytes) ||
          !AddWithinLimit(admitted_bytes, cell.canonical_payload.size(),
                          packet_limit, &admitted_bytes)) {
        return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                          kResourceLimitExceeded, "batch_frame_too_large");
      }
    }
  }
  TypedResultRowDescriptor canonical_descriptor;
  const auto descriptor_result =
      DescriptorForBatch(descriptor, &canonical_descriptor);
  if (!descriptor_result.ok()) {
    return descriptor_result;
  }
  if (!BatchSystemIdentitiesValid(batch, carrier_binding))
    return BatchError(TypedResultCodecStatus::invalid_argument,
                      kSystemUuidInvalid, "batch_system_uuid_invalid");
  if (!UuidPresent(batch.execution_uuid) ||
      !UuidPresent(batch.result_set_uuid) || !UuidPresent(batch.batch_uuid) ||
      !UuidPresent(batch.snapshot_uuid)) {
    return BatchError(TypedResultCodecStatus::invalid_argument,
                      kFrameInvalid,
                      "query_result_handle_and_batch_identity_required");
  }
  if (batch.row_descriptor_uuid != canonical_descriptor.descriptor_uuid ||
      batch.row_descriptor_generation !=
          canonical_descriptor.descriptor_generation) {
    return BatchError(TypedResultCodecStatus::descriptor_mismatch,
                      kDatatypeDescriptorInvalid,
                      "batch_row_descriptor_identity_mismatch");
  }
  if (HashPresent(batch.descriptor_evidence_sha256) &&
      !SameHash(batch.descriptor_evidence_sha256,
                canonical_descriptor.descriptor_evidence_sha256)) {
    return BatchError(TypedResultCodecStatus::descriptor_mismatch,
                      kDatatypeDescriptorInvalid,
                      "batch_row_descriptor_evidence_mismatch");
  }
  if (batch.rows.empty()) {
    return BatchError(TypedResultCodecStatus::shape_invalid, kFrameInvalid,
                      "nonempty_packet_requires_rows");
  }
  if (batch.rows.size() > kMaxRowCount) {
    return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded,
                      "batch_row_count_limit_exceeded");
  }
  const auto carrier_result = ValidateCarrierBinding(batch, carrier_binding, batch.rows.size());
  if (!carrier_result.ok()) {
    return carrier_result;
  }

  output.reserve(static_cast<std::size_t>(admitted_bytes));
  output.assign(kTypedResultBatchHeaderBytes, 0);
  auto& rows = output;
  for (std::size_t row_index = 0; row_index < batch.rows.size(); ++row_index) {
    const auto& row = batch.rows[row_index];
    if (row.row_ordinal != row_index ||
        row.cells.size() != canonical_descriptor.columns.size()) {
      return BatchError(TypedResultCodecStatus::shape_invalid, kFrameInvalid,
                        "row_ordinal_or_cell_count_mismatch");
    }
    const std::size_t row_begin = rows.size();
    rows.resize(row_begin + kRowFrameFixedBytes);
    for (std::size_t cell_index = 0; cell_index < row.cells.size();
         ++cell_index) {
      const auto& cell = row.cells[cell_index];
      const auto& column = canonical_descriptor.columns[cell_index];
      if (!CellMatchesColumn(cell, column) || !ValidValueState(cell.state)) {
        return BatchError(TypedResultCodecStatus::shape_invalid,
                          kFrameInvalid,
                          "cell_column_identity_mismatch");
      }
      if (cell.state == TypedResultValueState::sql_null &&
          (column.nullability == TypedResultNullability::not_null ||
           !cell.canonical_payload.empty())) {
        return BatchError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          "invalid_sql_null_cell");
      }
      const u64 value_bytes = datatypes::kDatatypeBinaryEnvelopeHeaderBytes +
                              static_cast<u64>(cell.canonical_payload.size());
      const u64 cell_bytes_u64 =
          static_cast<u64>(kCellFrameFixedBytes) + value_bytes;
      if (cell_bytes_u64 > std::numeric_limits<u32>::max()) {
        return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                          kResourceLimitExceeded, "cell_frame_too_large");
      }
      const auto cell_begin = rows.size();
      rows.resize(cell_begin + kCellFrameFixedBytes);
      WriteCellHeader(cell, static_cast<u32>(value_bytes), rows.data() + cell_begin);
      const auto value_offset = rows.size();
      rows.resize(value_offset + static_cast<std::size_t>(value_bytes));
      const auto encoded_value = datatypes::EncodeDatatypeBinaryValueInto(
          {column.canonical_type_id,
           cell.state == TypedResultValueState::sql_null, false,
           cell.canonical_payload.data(), cell.canonical_payload.size()},
          rows.data() + value_offset, static_cast<std::size_t>(value_bytes));
      if (!encoded_value.ok() || encoded_value.bytes_written != value_bytes) {
        return BatchError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          encoded_value.diagnostic.diagnostic_code);
      }
    }
    const u64 row_bytes_u64 = rows.size() - row_begin;
    if (row_bytes_u64 > std::numeric_limits<u32>::max()) {
      return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                        kResourceLimitExceeded, "row_frame_too_large");
    }
    WriteRowHeader(static_cast<u32>(row_bytes_u64), static_cast<u32>(row.cells.size()),
                   row.row_ordinal, rows.data() + row_begin);
  }

  const u64 total_bytes = output.size();
  if (total_bytes > packet_limit || total_bytes != admitted_bytes) {
    return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded, "batch_frame_too_large");
  }

  TypedResultBatchCodecResult result;
  WriteBatchHeader(batch, canonical_descriptor, static_cast<u32>(batch.rows.size()),
                   output.size() - kTypedResultBatchHeaderBytes, output.data());

  const auto digest = ComputeEvidence(kBatchEvidenceDomain, output, kBatchEvidenceOffset);
  if (!digest.ok()) {
    return BatchError(TypedResultCodecStatus::invalid_argument,
                      kFrameInvalid, digest.diagnostic.diagnostic_code);
  }
  const auto evidence = DigestHash(digest);
  if (HashPresent(batch.batch_evidence_sha256) &&
      !SameHash(batch.batch_evidence_sha256, evidence)) {
    return BatchError(TypedResultCodecStatus::evidence_mismatch,
                      kFrameInvalid,
                      "provided_batch_evidence_mismatch");
  }
  std::copy(evidence.begin(), evidence.end(),
            output.begin() + kBatchEvidenceOffset);

  result.status = TypedResultCodecStatus::ok;
  result.batch.descriptor_evidence_sha256 =
      canonical_descriptor.descriptor_evidence_sha256;
  result.batch.batch_evidence_sha256 = evidence;
  return result;
}

template <class Batch>
TypedResultBatchCodecResult EncodeOwnedTypedResultBatch(
    Batch&& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    u64 maximum_bytes) {
  std::vector<byte> output;
  auto result = EncodeTypedResultBatchStorage(
      batch, descriptor, carrier_binding, maximum_bytes, output);
  if (!result.ok()) return result;
  const auto descriptor_hash = result.batch.descriptor_evidence_sha256;
  const auto batch_hash = result.batch.batch_evidence_sha256;
  // All fallible validation, hashing and packet allocation precede a consuming
  // caller's ownership transfer. The const overload retains its copy contract.
  static_assert(std::is_nothrow_move_assignable_v<TypedResultBatch>);
  result.batch = std::forward<Batch>(batch);
  result.batch.descriptor_evidence_sha256 = descriptor_hash;
  result.batch.batch_evidence_sha256 = batch_hash;
  result.encoded = std::move(output);
  return result;
}

}  // namespace

TypedResultBatchCodecResult EncodeTypedResultBatch(
    const TypedResultBatch& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    u64 maximum_bytes) {
  return EncodeOwnedTypedResultBatch(batch, descriptor, carrier_binding, maximum_bytes);
}

TypedResultBatchCodecResult EncodeTypedResultBatch(
    TypedResultBatch&& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    u64 maximum_bytes) {
  return EncodeOwnedTypedResultBatch(std::move(batch), descriptor, carrier_binding, maximum_bytes);
}

TypedResultResourceBuffer EncodeTypedResultBatchBuffer(
    const TypedResultBatch& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    std::pmr::memory_resource& resource,
    u64 maximum_bytes) {
  TypedResultResourceBuffer result(resource);
  std::pmr::vector<byte> staged(&resource);
  auto metadata = EncodeTypedResultBatchStorage(
      batch, descriptor, carrier_binding, maximum_bytes, staged);
  result.status = metadata.status;
  result.diagnostic_code = std::move(metadata.diagnostic_code);
  result.detail = std::move(metadata.detail);
  if (!result.ok()) return result;
  result.descriptor_evidence_sha256 = metadata.batch.descriptor_evidence_sha256;
  result.batch_evidence_sha256 = metadata.batch.batch_evidence_sha256;
  result.encoded = std::move(staged);
  return result;
}

TypedResultPacketViewResult DecodeTypedResultPacketView(
    const byte* encoded_data, std::size_t encoded_size,
    const TypedResultRowDescriptor& expected_descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    const TypedResultCursorBatchState* cursor_state) {
  if (encoded_data == nullptr && encoded_size != 0)
    return PacketError(TypedResultCodecStatus::malformed_frame, kFrameInvalid, "batch_storage_missing");
  const BorrowedByteSequence encoded{encoded_data, encoded_size};
  if (encoded.size() < kTypedResultBatchHeaderBytes) {
    return PacketError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_size_invalid");
  }
  if (encoded.size() > kMaxTransportFrameBytes) {
    return PacketError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded,
                      "batch_size_limit_exceeded");
  }
  if (!std::equal(kBatchMagic.begin(), kBatchMagic.end(), encoded.begin())) {
    return PacketError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_magic_invalid");
  }
  const u16 version = LoadLittle16(encoded.data() + 8);
  if (version != kTypedResultTransportVersion) {
    return PacketError(TypedResultCodecStatus::unsupported_version,
                      kFrameInvalid, "batch_version_unsupported");
  }
  const u16 header_bytes = LoadLittle16(encoded.data() + 10);
  const u32 flags = LoadLittle32(encoded.data() + 12);
  const u64 total_bytes = LoadLittle64(encoded.data() + 16);
  const u32 row_count = LoadLittle32(encoded.data() + 136);
  const u32 column_count = LoadLittle32(encoded.data() + 140);
  const u64 rows_bytes = LoadLittle64(encoded.data() + 144);
  const u64 reserved = LoadLittle64(encoded.data() + 184);
  if (row_count > kMaxRowCount || column_count > kMaxColumnCount) {
    return PacketError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded,
                      "batch_count_limit_exceeded");
  }
  if (header_bytes != kTypedResultBatchHeaderBytes || (flags & ~3u) != 0 ||
      total_bytes != encoded.size() ||
      rows_bytes != encoded.size() - header_bytes || row_count == 0 ||
      column_count == 0 || reserved != 0) {
    return PacketError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_header_invalid");
  }

  TypedResultEvidenceHash expected_batch_evidence{};
  std::copy_n(encoded.begin() + kBatchEvidenceOffset,
              expected_batch_evidence.size(),
              expected_batch_evidence.begin());
  const auto digest = ComputeEvidence(kBatchEvidenceDomain, encoded, kBatchEvidenceOffset);
  if (!digest.ok()) {
    return PacketError(TypedResultCodecStatus::invalid_argument,
                      kFrameInvalid, digest.diagnostic.diagnostic_code);
  }
  if (!SameHash(expected_batch_evidence, DigestHash(digest))) {
    return PacketError(TypedResultCodecStatus::evidence_mismatch,
                      kFrameInvalid, "batch_sha256_mismatch");
  }

  TypedResultRowDescriptor canonical_descriptor;
  const auto descriptor_result =
      DescriptorForBatch(expected_descriptor, &canonical_descriptor);
  if (!descriptor_result.ok()) {
    return PacketError(descriptor_result.status, descriptor_result.diagnostic_code, descriptor_result.detail);
  }

  TypedResultBatch batch;
  std::size_t header_offset = 24;
  if (!ReadUuid(encoded, &header_offset, &batch.execution_uuid) ||
      !ReadUuid(encoded, &header_offset, &batch.result_set_uuid) ||
      !ReadUuid(encoded, &header_offset, &batch.batch_uuid) ||
      !ReadU64(encoded, &header_offset, &batch.batch_ordinal) ||
      !ReadUuid(encoded, &header_offset, &batch.row_descriptor_uuid) ||
      !ReadU64(encoded, &header_offset, &batch.row_descriptor_generation)) {
    return PacketError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_identity_truncated");
  }
  std::copy_n(encoded.begin() + 104,
              batch.descriptor_evidence_sha256.size(),
              batch.descriptor_evidence_sha256.begin());
  std::copy_n(encoded.begin() + 152, batch.snapshot_uuid.size(),
              batch.snapshot_uuid.begin());
  std::copy_n(encoded.begin() + 168, batch.cursor_uuid.size(),
              batch.cursor_uuid.begin());
  batch.batch_evidence_sha256 = expected_batch_evidence;
  batch.end_of_rowset = (flags & 1u) != 0;
  batch.cursor_bound = (flags & 2u) != 0;

  if (!BatchSystemIdentitiesValid(batch, carrier_binding))
    return PacketError(TypedResultCodecStatus::malformed_frame,
                      kSystemUuidInvalid, "batch_system_uuid_invalid");

  if (!UuidPresent(batch.execution_uuid) ||
      !UuidPresent(batch.result_set_uuid) || !UuidPresent(batch.batch_uuid) ||
      !UuidPresent(batch.snapshot_uuid)) {
    return PacketError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_identity_missing");
  }
  if (batch.row_descriptor_uuid != canonical_descriptor.descriptor_uuid ||
      batch.row_descriptor_generation !=
          canonical_descriptor.descriptor_generation ||
      !SameHash(batch.descriptor_evidence_sha256,
                canonical_descriptor.descriptor_evidence_sha256) ||
      column_count != canonical_descriptor.columns.size()) {
    return PacketError(TypedResultCodecStatus::descriptor_mismatch,
                      kDatatypeDescriptorInvalid,
                      "batch_descriptor_binding_mismatch");
  }

  std::size_t offset = header_bytes;
  for (u32 row_index = 0; row_index < row_count; ++row_index) {
    const std::size_t row_begin = offset;
    u32 row_bytes = 0;
    u32 cell_count = 0;
    TypedResultRow row;
    if (!ReadU32(encoded, &offset, &row_bytes) ||
        row_bytes < kRowFrameFixedBytes || row_begin > encoded.size() ||
        row_bytes > encoded.size() - row_begin ||
        !ReadU32(encoded, &offset, &cell_count) ||
        !ReadU64(encoded, &offset, &row.row_ordinal) ||
        row.row_ordinal != row_index || cell_count != column_count) {
      return PacketError(TypedResultCodecStatus::shape_invalid,
                        kFrameInvalid, "row_frame_shape_invalid");
    }
    const std::size_t row_end = row_begin + row_bytes;
    for (u32 cell_index = 0; cell_index < cell_count; ++cell_index) {
      const std::size_t cell_begin = offset;
      u32 cell_bytes = 0;
      u32 value_bytes = 0;
      std::uint8_t value_state = 0;
      std::uint8_t value_encoding = 0;
      u16 reserved = 0;
      TypedResultCell cell;
      if (!ReadU32(encoded, &offset, &cell_bytes) ||
          cell_bytes < kCellFrameFixedBytes || cell_begin > row_end ||
          cell_bytes > row_end - cell_begin ||
          !ReadU32(encoded, &offset, &cell.column_ordinal) ||
          !ReadU32(encoded, &offset, &cell.name_occurrence) ||
          !ReadU8(encoded, &offset, &value_state) ||
          !ReadU8(encoded, &offset, &value_encoding) ||
          !ReadU16(encoded, &offset, &reserved) ||
          !ReadU32(encoded, &offset, &value_bytes) ||
          value_encoding != kDatatypeBinaryValueEncoding || reserved != 0 ||
          value_bytes != cell_bytes - kCellFrameFixedBytes ||
          value_bytes > row_end - offset) {
        return PacketError(TypedResultCodecStatus::malformed_frame,
                          kFrameInvalid, "cell_frame_invalid");
      }
      cell.state = static_cast<TypedResultValueState>(value_state);
      const auto& column = canonical_descriptor.columns[cell_index];
      if (!ValidValueState(cell.state) || !CellMatchesColumn(cell, column)) {
        return PacketError(TypedResultCodecStatus::shape_invalid,
                          kFrameInvalid,
                          "cell_column_identity_mismatch");
      }
      const byte* encoded_value = encoded.data() + offset;
      offset += value_bytes;
      if (offset != cell_begin + cell_bytes ||
          value_bytes < datatypes::kDatatypeBinaryEnvelopeHeaderBytes ||
          LoadLittle16(encoded_value + 14) !=
              datatypes::kDatatypeBinaryEnvelopeHeaderBytes ||
          (LoadLittle16(encoded_value + 12) & ~1u) != 0 ||
          LoadLittle32(encoded_value + 20) != 0) {
        return PacketError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          "datatype_value_envelope_forbidden_or_malformed");
      }
      const auto decoded_value =
          datatypes::DecodeDatatypeBinaryValueView(encoded_value, value_bytes);
      if (!decoded_value.ok() ||
          decoded_value.value.type_id != column.canonical_type_id ||
          decoded_value.value.payload_is_toast_reference ||
          decoded_value.value.is_null !=
              (cell.state == TypedResultValueState::sql_null) ||
          (cell.state == TypedResultValueState::sql_null &&
           (column.nullability == TypedResultNullability::not_null ||
            decoded_value.value.payload_bytes != 0))) {
        return PacketError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          decoded_value.ok()
                              ? "cell_type_state_or_reference_mismatch"
                              : decoded_value.diagnostic.diagnostic_code);
      }
      std::array<byte, kCellFrameFixedBytes> canonical_cell{};
      WriteCellHeader(cell, value_bytes, canonical_cell.data());
      if (std::memcmp(canonical_cell.data(), encoded.data() + cell_begin, canonical_cell.size()) != 0)
        return PacketError(TypedResultCodecStatus::malformed_frame, kFrameInvalid,
                           "cell_noncanonical_reencode");
    }
    if (offset != row_end) {
      return PacketError(TypedResultCodecStatus::shape_invalid,
                        kFrameInvalid, "row_frame_length_mismatch");
    }
    std::array<byte, kRowFrameFixedBytes> canonical_row{};
    WriteRowHeader(row_bytes, cell_count, row.row_ordinal, canonical_row.data());
    if (std::memcmp(canonical_row.data(), encoded.data() + row_begin, canonical_row.size()) != 0)
      return PacketError(TypedResultCodecStatus::malformed_frame, kFrameInvalid,
                         "row_noncanonical_reencode");
  }
  if (offset != encoded.size()) {
    return PacketError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_trailing_bytes");
  }

  const auto carrier_result = ValidateCarrierBinding(batch, carrier_binding, row_count);
  if (!carrier_result.ok()) {
    return PacketError(carrier_result.status, carrier_result.diagnostic_code, carrier_result.detail);
  }
  if (carrier_binding.kind == TypedResultCarrierKind::ps_fetch_result_v1 &&
      cursor_state == nullptr) {
    return PacketError(TypedResultCodecStatus::invalid_argument,
                      kSequenceInvalid,
                      "fetch_decode_requires_cursor_batch_state");
  }

  std::array<byte, kTypedResultBatchHeaderBytes> canonical_header{};
  WriteBatchHeader(batch, canonical_descriptor, row_count, rows_bytes, canonical_header.data());
  std::copy(batch.batch_evidence_sha256.begin(), batch.batch_evidence_sha256.end(),
            canonical_header.begin() + kBatchEvidenceOffset);
  if (std::memcmp(canonical_header.data(), encoded.data(), canonical_header.size()) != 0)
    return PacketError(TypedResultCodecStatus::malformed_frame, kFrameInvalid,
                       "batch_noncanonical_reencode");

  TypedResultCursorBatchState next_state;
  if (cursor_state != nullptr) {
    if (!batch.cursor_bound) {
      return PacketError(TypedResultCodecStatus::invalid_argument,
                        kSequenceInvalid,
                        "cursor_state_requires_cursor_bound_batch");
    }
    next_state = *cursor_state;
    if (next_state.initialized) {
      if (next_state.terminal) {
        return PacketError(TypedResultCodecStatus::sequence_mismatch,
                          kSequenceInvalid,
                          "batch_after_terminal_cursor_batch");
      }
      if (batch.cursor_uuid != next_state.cursor_uuid ||
          batch.execution_uuid != next_state.execution_uuid ||
          batch.result_set_uuid != next_state.result_set_uuid ||
          batch.snapshot_uuid != next_state.snapshot_uuid) {
        return PacketError(TypedResultCodecStatus::cursor_mismatch,
                          kConnectionMismatch,
                          "cursor_stream_execution_or_snapshot_drift");
      }
      if (carrier_binding.cursor_stream_descriptor_uuid !=
              next_state.cursor_stream_descriptor_uuid ||
          carrier_binding.cursor_stream_descriptor_version !=
              next_state.cursor_stream_descriptor_version ||
          carrier_binding.cursor_stream_descriptor_generation !=
              next_state.cursor_stream_descriptor_generation) {
        return PacketError(TypedResultCodecStatus::cursor_mismatch,
                          kConnectionMismatch,
                          "cursor_stream_descriptor_drift");
      }
      if (batch.row_descriptor_uuid != next_state.row_descriptor_uuid ||
          batch.row_descriptor_generation !=
              next_state.row_descriptor_generation ||
          !SameHash(batch.descriptor_evidence_sha256,
                    next_state.descriptor_evidence_sha256)) {
        return PacketError(TypedResultCodecStatus::descriptor_mismatch,
                          kDatatypeDescriptorInvalid,
                          "cursor_row_descriptor_drift");
      }
      if (batch.batch_ordinal != next_state.next_batch_ordinal) {
        return PacketError(TypedResultCodecStatus::sequence_mismatch,
                          kSequenceInvalid,
                          "cursor_batch_ordinal_not_contiguous");
      }
    } else {
      if (batch.batch_ordinal != 0) {
        return PacketError(TypedResultCodecStatus::sequence_mismatch,
                          kSequenceInvalid,
                          "cursor_first_batch_ordinal_not_zero");
      }
      next_state.initialized = true;
      next_state.cursor_uuid = batch.cursor_uuid;
      next_state.execution_uuid = batch.execution_uuid;
      next_state.result_set_uuid = batch.result_set_uuid;
      next_state.snapshot_uuid = batch.snapshot_uuid;
      next_state.cursor_stream_descriptor_uuid =
          carrier_binding.cursor_stream_descriptor_uuid;
      next_state.cursor_stream_descriptor_version =
          carrier_binding.cursor_stream_descriptor_version;
      next_state.cursor_stream_descriptor_generation =
          carrier_binding.cursor_stream_descriptor_generation;
      next_state.row_descriptor_uuid = batch.row_descriptor_uuid;
      next_state.row_descriptor_generation =
          batch.row_descriptor_generation;
      next_state.descriptor_evidence_sha256 =
          batch.descriptor_evidence_sha256;
    }
    if (std::find(next_state.seen_batch_uuids.begin(),
                  next_state.seen_batch_uuids.end(),
                  batch.batch_uuid) != next_state.seen_batch_uuids.end()) {
      return PacketError(TypedResultCodecStatus::sequence_mismatch,
                        kSequenceInvalid,
                        "cursor_batch_uuid_reused");
    }
    if (batch.batch_ordinal == std::numeric_limits<u64>::max() &&
        !batch.end_of_rowset) {
      return PacketError(TypedResultCodecStatus::sequence_mismatch,
                        kSequenceInvalid,
                        "cursor_batch_ordinal_overflow");
    }
    next_state.next_batch_ordinal = batch.batch_ordinal + 1u;
    next_state.terminal = batch.end_of_rowset;
    next_state.seen_batch_uuids.push_back(batch.batch_uuid);
  }

  TypedResultPacketViewResult result;
  result.status = TypedResultCodecStatus::ok;
  result.view.data_ = encoded_data;
  result.view.size_ = encoded_size;
  result.view.header_ = PacketHeaderFromBatch(batch, row_count, column_count);
  if (cursor_state != nullptr) {
    result.has_cursor_state = true;
    result.next_cursor_state = std::move(next_state);
  }
  return result;
}

bool TypedResultPacketRowCursor::Next(TypedResultPacketRowView* row) noexcept {
  if (row == nullptr || remaining_ == 0) return false;
  row->ordinal_ = LoadLittle64(next_ + 8);
  row->cell_count_ = LoadLittle32(next_ + 4);
  row->cells_ = next_ + kRowFrameFixedBytes;
  next_ += LoadLittle32(next_);
  --remaining_;
  return true;
}

bool TypedResultPacketCellCursor::Next(TypedResultPacketCellView* cell) noexcept {
  if (cell == nullptr || remaining_ == 0) return false;
  cell->column_ordinal = LoadLittle32(next_ + 4);
  cell->name_occurrence = LoadLittle32(next_ + 8);
  cell->state = static_cast<TypedResultValueState>(next_[12]);
  const byte* value = next_ + kCellFrameFixedBytes;
  cell->value.type_id = static_cast<CanonicalTypeId>(LoadLittle32(value + 8));
  const u16 flags = LoadLittle16(value + 12);
  cell->value.is_null = (flags & 1u) != 0;
  cell->value.payload_is_toast_reference = (flags & 2u) != 0;
  cell->value.payload_data = value + datatypes::kDatatypeBinaryEnvelopeHeaderBytes;
  cell->value.payload_bytes = LoadLittle32(value + 16);
  next_ += LoadLittle32(next_);
  --remaining_;
  return true;
}

TypedResultBatchCodecResult DecodeTypedResultBatch(
    const std::vector<byte>& encoded,
    const TypedResultRowDescriptor& expected_descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    TypedResultCursorBatchState* cursor_state) {
  auto parsed = DecodeTypedResultPacketView(encoded.data(), encoded.size(),
      expected_descriptor, carrier_binding, cursor_state);
  if (!parsed.ok())
    return BatchError(parsed.status, std::move(parsed.diagnostic_code), std::move(parsed.detail));
  TypedResultBatchCodecResult result;
  result.batch = BatchFromPacketHeader(parsed.view.header());
  result.batch.rows.reserve(parsed.view.header().row_count);
  auto rows = parsed.view.rows();
  TypedResultPacketRowView source_row;
  while (rows.Next(&source_row)) {
    TypedResultRow row;
    row.row_ordinal = source_row.row_ordinal();
    row.cells.reserve(source_row.cell_count());
    auto cells = source_row.cells();
    TypedResultPacketCellView source_cell;
    while (cells.Next(&source_cell)) {
      TypedResultCell cell;
      cell.column_ordinal = source_cell.column_ordinal;
      cell.name_occurrence = source_cell.name_occurrence;
      cell.state = source_cell.state;
      cell.canonical_payload.assign(source_cell.value.payload_data,
          source_cell.value.payload_data + source_cell.value.payload_bytes);
      row.cells.push_back(std::move(cell));
    }
    result.batch.rows.push_back(std::move(row));
  }
  result.encoded = encoded;
  // No caller state advances until all owning result copies exist.
  static_assert(std::is_nothrow_move_assignable_v<TypedResultCursorBatchState>);
  if (cursor_state != nullptr)
    *cursor_state = std::move(parsed.next_cursor_state);
  result.status = TypedResultCodecStatus::ok;
  return result;
}

}  // namespace scratchbird::wire
