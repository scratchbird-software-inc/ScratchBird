// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "typed_result_transport_codec.hpp"

#include "datatype_binary.hpp"
#include "datatype_layout.hpp"
#include "hash_digest.hpp"

#include <algorithm>
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
constexpr u32 kColumnDescriptorFixedBytes = 92;
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
    "DATATYPE.DESCRIPTOR_INVALID";
constexpr const char* kConnectionMismatch =
    "PARSER_SERVER_IPC.CONNECTION_MISMATCH";
constexpr const char* kSequenceInvalid =
    "PARSER_SERVER_IPC.SEQUENCE_INVALID";

enum class DescriptorValidationKind {
  ok,
  frame,
  datatype,
  resource,
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

bool HashPresent(const TypedResultEvidenceHash& hash) {
  return std::any_of(hash.begin(), hash.end(), [](byte value) {
    return value != 0;
  });
}

bool SameHash(const TypedResultEvidenceHash& left,
              const TypedResultEvidenceHash& right) {
  const std::vector<byte> left_bytes(left.begin(), left.end());
  const std::vector<byte> right_bytes(right.begin(), right.end());
  return core_hash::ConstantTimeEqual(left_bytes, right_bytes);
}

bool ValidUtf8(std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first == 0) {
      return false;
    }
    if (first <= 0x7fu) {
      ++offset;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first >= 0xc2u && first <= 0xdfu) {
      continuation_count = 1;
      code_point = first & 0x1fu;
    } else if (first >= 0xe0u && first <= 0xefu) {
      continuation_count = 2;
      code_point = first & 0x0fu;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      continuation_count = 3;
      code_point = first & 0x07u;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1u) {
      return false;
    }
    for (std::size_t index = 0; index < continuation_count; ++index) {
      const auto next =
          static_cast<unsigned char>(value[offset + index + 1u]);
      if ((next & 0xc0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6u) | (next & 0x3fu);
    }
    if ((continuation_count == 2u && code_point < 0x800u) ||
        (continuation_count == 3u && code_point < 0x10000u) ||
        (code_point >= 0xd800u && code_point <= 0xdfffu) ||
        code_point > 0x10ffffu) {
      return false;
    }
    offset += continuation_count + 1u;
  }
  return true;
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

void AppendU16(std::vector<byte>* out, u16 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
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

bool ReadU8(const std::vector<byte>& bytes,
            std::size_t* offset,
            std::uint8_t* value) {
  if (*offset >= bytes.size()) {
    return false;
  }
  *value = bytes[(*offset)++];
  return true;
}

bool ReadU16(const std::vector<byte>& bytes,
             std::size_t* offset,
             u16* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < sizeof(*value)) {
    return false;
  }
  *value = LoadLittle16(bytes.data() + *offset);
  *offset += sizeof(*value);
  return true;
}

bool ReadU32(const std::vector<byte>& bytes,
             std::size_t* offset,
             u32* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < sizeof(*value)) {
    return false;
  }
  *value = LoadLittle32(bytes.data() + *offset);
  *offset += sizeof(*value);
  return true;
}

bool ReadU64(const std::vector<byte>& bytes,
             std::size_t* offset,
             u64* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < sizeof(*value)) {
    return false;
  }
  *value = LoadLittle64(bytes.data() + *offset);
  *offset += sizeof(*value);
  return true;
}

bool ReadUuid(const std::vector<byte>& bytes,
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

bool ReadString(const std::vector<byte>& bytes,
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
    if (column.ordinal != index) {
      return {DescriptorValidationKind::frame,
              "column_ordinal_not_contiguous"};
    }
    if (column.name.size() > kMaxColumnNameBytes ||
        !ValidUtf8(column.name)) {
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
    if (layout.layout.storage_class ==
            datatypes::DatatypeStorageClass::inline_fixed &&
        column.canonical_value_bytes != layout.layout.inline_bytes) {
      return {DescriptorValidationKind::datatype,
              "column_fixed_width_mismatch"};
    }
    if (layout.layout.storage_class !=
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

core_hash::HashDigestResult ComputeEvidence(
    std::string_view domain,
    const std::vector<byte>& canonical_bytes) {
  std::vector<byte> material;
  material.reserve(domain.size() + canonical_bytes.size());
  for (const char character : domain) {
    material.push_back(static_cast<byte>(character));
  }
  material.insert(material.end(), canonical_bytes.begin(),
                  canonical_bytes.end());
  return core_hash::ComputeSha256Digest(material);
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
    const TypedResultCarrierBinding& carrier_binding) {
  if (carrier_binding.row_count != batch.rows.size() ||
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
  }
  return {};
}

}  // namespace

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
                                      result.encoded);
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
  auto evidence_bytes = encoded;
  std::fill(evidence_bytes.begin() + kDescriptorEvidenceOffset,
            evidence_bytes.begin() + kDescriptorEvidenceOffset +
                kTypedResultEvidenceHashBytes,
            0);
  const auto digest = ComputeEvidence(kDescriptorEvidenceDomain,
                                      evidence_bytes);
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

TypedResultBatchCodecResult EncodeTypedResultBatch(
    const TypedResultBatch& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding) {
  TypedResultRowDescriptor canonical_descriptor;
  const auto descriptor_result =
      DescriptorForBatch(descriptor, &canonical_descriptor);
  if (!descriptor_result.ok()) {
    return descriptor_result;
  }
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
  const auto carrier_result = ValidateCarrierBinding(batch, carrier_binding);
  if (!carrier_result.ok()) {
    return carrier_result;
  }

  std::vector<byte> rows;
  for (std::size_t row_index = 0; row_index < batch.rows.size(); ++row_index) {
    const auto& row = batch.rows[row_index];
    if (row.row_ordinal != row_index ||
        row.cells.size() != canonical_descriptor.columns.size()) {
      return BatchError(TypedResultCodecStatus::shape_invalid, kFrameInvalid,
                        "row_ordinal_or_cell_count_mismatch");
    }
    const std::size_t row_begin = rows.size();
    AppendU32(&rows, 0);
    AppendU32(&rows, static_cast<u32>(row.cells.size()));
    AppendU64(&rows, row.row_ordinal);
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
      datatypes::DatatypeBinaryValue value;
      value.type_id = column.canonical_type_id;
      value.is_null = cell.state == TypedResultValueState::sql_null;
      value.payload = cell.canonical_payload;
      const auto encoded_value = datatypes::EncodeDatatypeBinaryValue(value);
      if (!encoded_value.ok()) {
        return BatchError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          encoded_value.diagnostic.diagnostic_code);
      }
      const u64 cell_bytes_u64 =
          static_cast<u64>(kCellFrameFixedBytes) + encoded_value.encoded.size();
      if (cell_bytes_u64 > std::numeric_limits<u32>::max()) {
        return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                          kResourceLimitExceeded, "cell_frame_too_large");
      }
      AppendU32(&rows, static_cast<u32>(cell_bytes_u64));
      AppendU32(&rows, cell.column_ordinal);
      AppendU32(&rows, cell.name_occurrence);
      rows.push_back(static_cast<byte>(cell.state));
      rows.push_back(kDatatypeBinaryValueEncoding);
      AppendU16(&rows, 0);
      AppendU32(&rows, static_cast<u32>(encoded_value.encoded.size()));
      rows.insert(rows.end(), encoded_value.encoded.begin(),
                  encoded_value.encoded.end());
    }
    const u64 row_bytes_u64 = rows.size() - row_begin;
    if (row_bytes_u64 > std::numeric_limits<u32>::max()) {
      return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                        kResourceLimitExceeded, "row_frame_too_large");
    }
    StoreLittle32(rows.data() + row_begin, static_cast<u32>(row_bytes_u64));
  }

  u64 total_bytes = 0;
  if (!AddWithinLimit(kTypedResultBatchHeaderBytes, rows.size(),
                      kMaxTransportFrameBytes, &total_bytes)) {
    return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded, "batch_frame_too_large");
  }

  TypedResultBatchCodecResult result;
  result.encoded.assign(kTypedResultBatchHeaderBytes, 0);
  std::copy(kBatchMagic.begin(), kBatchMagic.end(), result.encoded.begin());
  StoreLittle16(result.encoded.data() + 8, kTypedResultTransportVersion);
  StoreLittle16(result.encoded.data() + 10, kTypedResultBatchHeaderBytes);
  const u32 flags = (batch.end_of_rowset ? 1u : 0u) |
                    (batch.cursor_bound ? 2u : 0u);
  StoreLittle32(result.encoded.data() + 12, flags);
  StoreLittle64(result.encoded.data() + 16, total_bytes);
  std::copy(batch.execution_uuid.begin(), batch.execution_uuid.end(),
            result.encoded.begin() + 24);
  std::copy(batch.result_set_uuid.begin(), batch.result_set_uuid.end(),
            result.encoded.begin() + 40);
  std::copy(batch.batch_uuid.begin(), batch.batch_uuid.end(),
            result.encoded.begin() + 56);
  StoreLittle64(result.encoded.data() + 72, batch.batch_ordinal);
  std::copy(canonical_descriptor.descriptor_uuid.begin(),
            canonical_descriptor.descriptor_uuid.end(),
            result.encoded.begin() + 80);
  StoreLittle64(result.encoded.data() + 96,
                canonical_descriptor.descriptor_generation);
  std::copy(canonical_descriptor.descriptor_evidence_sha256.begin(),
            canonical_descriptor.descriptor_evidence_sha256.end(),
            result.encoded.begin() + 104);
  StoreLittle32(result.encoded.data() + 136,
                static_cast<u32>(batch.rows.size()));
  StoreLittle32(result.encoded.data() + 140,
                static_cast<u32>(canonical_descriptor.columns.size()));
  StoreLittle64(result.encoded.data() + 144,
                static_cast<u64>(rows.size()));
  std::copy(batch.snapshot_uuid.begin(), batch.snapshot_uuid.end(),
            result.encoded.begin() + 152);
  std::copy(batch.cursor_uuid.begin(), batch.cursor_uuid.end(),
            result.encoded.begin() + 168);
  result.encoded.insert(result.encoded.end(), rows.begin(), rows.end());

  const auto digest = ComputeEvidence(kBatchEvidenceDomain, result.encoded);
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
            result.encoded.begin() + kBatchEvidenceOffset);

  result.status = TypedResultCodecStatus::ok;
  result.batch = batch;
  result.batch.descriptor_evidence_sha256 =
      canonical_descriptor.descriptor_evidence_sha256;
  result.batch.batch_evidence_sha256 = evidence;
  return result;
}

TypedResultBatchCodecResult DecodeTypedResultBatch(
    const std::vector<byte>& encoded,
    const TypedResultRowDescriptor& expected_descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    TypedResultCursorBatchState* cursor_state) {
  if (encoded.size() < kTypedResultBatchHeaderBytes) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_size_invalid");
  }
  if (encoded.size() > kMaxTransportFrameBytes) {
    return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded,
                      "batch_size_limit_exceeded");
  }
  if (!std::equal(kBatchMagic.begin(), kBatchMagic.end(), encoded.begin())) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_magic_invalid");
  }
  const u16 version = LoadLittle16(encoded.data() + 8);
  if (version != kTypedResultTransportVersion) {
    return BatchError(TypedResultCodecStatus::unsupported_version,
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
    return BatchError(TypedResultCodecStatus::resource_limit_exceeded,
                      kResourceLimitExceeded,
                      "batch_count_limit_exceeded");
  }
  if (header_bytes != kTypedResultBatchHeaderBytes || (flags & ~3u) != 0 ||
      total_bytes != encoded.size() ||
      rows_bytes != encoded.size() - header_bytes || row_count == 0 ||
      column_count == 0 || reserved != 0) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_header_invalid");
  }

  TypedResultEvidenceHash expected_batch_evidence{};
  std::copy_n(encoded.begin() + kBatchEvidenceOffset,
              expected_batch_evidence.size(),
              expected_batch_evidence.begin());
  auto evidence_bytes = encoded;
  std::fill(evidence_bytes.begin() + kBatchEvidenceOffset,
            evidence_bytes.begin() + kBatchEvidenceOffset +
                kTypedResultEvidenceHashBytes,
            0);
  const auto digest = ComputeEvidence(kBatchEvidenceDomain, evidence_bytes);
  if (!digest.ok()) {
    return BatchError(TypedResultCodecStatus::invalid_argument,
                      kFrameInvalid, digest.diagnostic.diagnostic_code);
  }
  if (!SameHash(expected_batch_evidence, DigestHash(digest))) {
    return BatchError(TypedResultCodecStatus::evidence_mismatch,
                      kFrameInvalid, "batch_sha256_mismatch");
  }

  TypedResultRowDescriptor canonical_descriptor;
  const auto descriptor_result =
      DescriptorForBatch(expected_descriptor, &canonical_descriptor);
  if (!descriptor_result.ok()) {
    return descriptor_result;
  }

  TypedResultBatch batch;
  std::size_t header_offset = 24;
  if (!ReadUuid(encoded, &header_offset, &batch.execution_uuid) ||
      !ReadUuid(encoded, &header_offset, &batch.result_set_uuid) ||
      !ReadUuid(encoded, &header_offset, &batch.batch_uuid) ||
      !ReadU64(encoded, &header_offset, &batch.batch_ordinal) ||
      !ReadUuid(encoded, &header_offset, &batch.row_descriptor_uuid) ||
      !ReadU64(encoded, &header_offset, &batch.row_descriptor_generation)) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
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

  if (!UuidPresent(batch.execution_uuid) ||
      !UuidPresent(batch.result_set_uuid) || !UuidPresent(batch.batch_uuid) ||
      !UuidPresent(batch.snapshot_uuid)) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_identity_missing");
  }
  if (batch.row_descriptor_uuid != canonical_descriptor.descriptor_uuid ||
      batch.row_descriptor_generation !=
          canonical_descriptor.descriptor_generation ||
      !SameHash(batch.descriptor_evidence_sha256,
                canonical_descriptor.descriptor_evidence_sha256) ||
      column_count != canonical_descriptor.columns.size()) {
    return BatchError(TypedResultCodecStatus::descriptor_mismatch,
                      kDatatypeDescriptorInvalid,
                      "batch_descriptor_binding_mismatch");
  }

  batch.rows.reserve(row_count);
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
      return BatchError(TypedResultCodecStatus::shape_invalid,
                        kFrameInvalid, "row_frame_shape_invalid");
    }
    const std::size_t row_end = row_begin + row_bytes;
    row.cells.reserve(cell_count);
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
        return BatchError(TypedResultCodecStatus::malformed_frame,
                          kFrameInvalid, "cell_frame_invalid");
      }
      cell.state = static_cast<TypedResultValueState>(value_state);
      const auto& column = canonical_descriptor.columns[cell_index];
      if (!ValidValueState(cell.state) || !CellMatchesColumn(cell, column)) {
        return BatchError(TypedResultCodecStatus::shape_invalid,
                          kFrameInvalid,
                          "cell_column_identity_mismatch");
      }
      std::vector<byte> encoded_value(
          encoded.begin() + static_cast<std::ptrdiff_t>(offset),
          encoded.begin() + static_cast<std::ptrdiff_t>(offset + value_bytes));
      offset += value_bytes;
      if (offset != cell_begin + cell_bytes ||
          encoded_value.size() < datatypes::kDatatypeBinaryEnvelopeHeaderBytes ||
          LoadLittle16(encoded_value.data() + 14) !=
              datatypes::kDatatypeBinaryEnvelopeHeaderBytes ||
          (LoadLittle16(encoded_value.data() + 12) & ~1u) != 0 ||
          LoadLittle32(encoded_value.data() + 20) != 0) {
        return BatchError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          "datatype_value_envelope_forbidden_or_malformed");
      }
      const auto decoded_value =
          datatypes::DecodeDatatypeBinaryValue(encoded_value);
      if (!decoded_value.ok() ||
          decoded_value.value.type_id != column.canonical_type_id ||
          decoded_value.value.payload_is_toast_reference ||
          decoded_value.value.is_null !=
              (cell.state == TypedResultValueState::sql_null) ||
          (cell.state == TypedResultValueState::sql_null &&
           (column.nullability == TypedResultNullability::not_null ||
            !decoded_value.value.payload.empty()))) {
        return BatchError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          decoded_value.ok()
                              ? "cell_type_state_or_reference_mismatch"
                              : decoded_value.diagnostic.diagnostic_code);
      }
      const auto canonical_value =
          datatypes::EncodeDatatypeBinaryValue(decoded_value.value);
      if (!canonical_value.ok() || canonical_value.encoded != encoded_value) {
        return BatchError(TypedResultCodecStatus::value_invalid,
                          kDatatypeDescriptorInvalid,
                          "datatype_value_noncanonical_reencode");
      }
      cell.canonical_payload = decoded_value.value.payload;
      row.cells.push_back(std::move(cell));
    }
    if (offset != row_end) {
      return BatchError(TypedResultCodecStatus::shape_invalid,
                        kFrameInvalid, "row_frame_length_mismatch");
    }
    batch.rows.push_back(std::move(row));
  }
  if (offset != encoded.size()) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_trailing_bytes");
  }

  const auto carrier_result = ValidateCarrierBinding(batch, carrier_binding);
  if (!carrier_result.ok()) {
    return carrier_result;
  }
  if (carrier_binding.kind == TypedResultCarrierKind::ps_fetch_result_v1 &&
      cursor_state == nullptr) {
    return BatchError(TypedResultCodecStatus::invalid_argument,
                      kSequenceInvalid,
                      "fetch_decode_requires_cursor_batch_state");
  }

  const auto canonical =
      EncodeTypedResultBatch(batch, canonical_descriptor, carrier_binding);
  if (!canonical.ok() || canonical.encoded != encoded) {
    return BatchError(TypedResultCodecStatus::malformed_frame,
                      kFrameInvalid, "batch_noncanonical_reencode");
  }

  TypedResultCursorBatchState next_state;
  if (cursor_state != nullptr) {
    if (!batch.cursor_bound) {
      return BatchError(TypedResultCodecStatus::invalid_argument,
                        kSequenceInvalid,
                        "cursor_state_requires_cursor_bound_batch");
    }
    next_state = *cursor_state;
    if (next_state.initialized) {
      if (next_state.terminal) {
        return BatchError(TypedResultCodecStatus::sequence_mismatch,
                          kSequenceInvalid,
                          "batch_after_terminal_cursor_batch");
      }
      if (batch.cursor_uuid != next_state.cursor_uuid ||
          batch.execution_uuid != next_state.execution_uuid ||
          batch.result_set_uuid != next_state.result_set_uuid ||
          batch.snapshot_uuid != next_state.snapshot_uuid) {
        return BatchError(TypedResultCodecStatus::cursor_mismatch,
                          kConnectionMismatch,
                          "cursor_stream_execution_or_snapshot_drift");
      }
      if (carrier_binding.cursor_stream_descriptor_uuid !=
              next_state.cursor_stream_descriptor_uuid ||
          carrier_binding.cursor_stream_descriptor_version !=
              next_state.cursor_stream_descriptor_version ||
          carrier_binding.cursor_stream_descriptor_generation !=
              next_state.cursor_stream_descriptor_generation) {
        return BatchError(TypedResultCodecStatus::cursor_mismatch,
                          kConnectionMismatch,
                          "cursor_stream_descriptor_drift");
      }
      if (batch.row_descriptor_uuid != next_state.row_descriptor_uuid ||
          batch.row_descriptor_generation !=
              next_state.row_descriptor_generation ||
          !SameHash(batch.descriptor_evidence_sha256,
                    next_state.descriptor_evidence_sha256)) {
        return BatchError(TypedResultCodecStatus::descriptor_mismatch,
                          kDatatypeDescriptorInvalid,
                          "cursor_row_descriptor_drift");
      }
      if (batch.batch_ordinal != next_state.next_batch_ordinal) {
        return BatchError(TypedResultCodecStatus::sequence_mismatch,
                          kSequenceInvalid,
                          "cursor_batch_ordinal_not_contiguous");
      }
    } else {
      if (batch.batch_ordinal != 0) {
        return BatchError(TypedResultCodecStatus::sequence_mismatch,
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
      return BatchError(TypedResultCodecStatus::sequence_mismatch,
                        kSequenceInvalid,
                        "cursor_batch_uuid_reused");
    }
    if (batch.batch_ordinal == std::numeric_limits<u64>::max() &&
        !batch.end_of_rowset) {
      return BatchError(TypedResultCodecStatus::sequence_mismatch,
                        kSequenceInvalid,
                        "cursor_batch_ordinal_overflow");
    }
    next_state.next_batch_ordinal = batch.batch_ordinal + 1u;
    next_state.terminal = batch.end_of_rowset;
    next_state.seen_batch_uuids.push_back(batch.batch_uuid);
  }

  TypedResultBatchCodecResult result;
  result.status = TypedResultCodecStatus::ok;
  result.encoded = encoded;
  result.batch = std::move(batch);
  if (cursor_state != nullptr) {
    *cursor_state = std::move(next_state);
  }
  return result;
}

}  // namespace scratchbird::wire
