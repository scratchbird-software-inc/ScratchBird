// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine {

enum class SblrOperationFamily : std::uint16_t {
  relational_query = 10,
  dml_insert = 20,
  dml_update = 21,
  dml_delete = 22,
  dml_merge = 23,
  catalog_mutation = 30,
  security_mutation = 40,
  transaction_control = 50,
  bulk_import = 60,
  bulk_export = 61,
  management_inspect = 70,
  management_control = 71,
  metrics_inspect = 80,
  replication_operation = 90,
  structured_kv = 100,
  document = 101,
  graph = 102,
  search = 103,
  vector = 104,
  timeseries = 105,
  versioned_history = 110,
  cluster_placement = 120,
  acceleration_management = 130,
  donor_meta = 65000,
};

enum class SblrBehaviorStatus : std::uint8_t {
  implemented = 1,
  admission_only = 2,
  noncluster_fail_closed = 3,
  edition_fail_closed = 4,
  capability_fail_closed = 5,
  deferred_to_successor = 6,
  unsupported = 7,
};

enum class SblrPayloadKind : std::uint16_t {
  opcode_stream = 1,
  operation_envelope = 2,
};

enum class SblrCodecStatus : std::uint8_t {
  ok = 0,
  envelope_invalid = 1,
  envelope_truncated = 2,
  checksum_invalid = 3,
  version_unsupported = 4,
  opcode_unknown = 5,
  donor_meta_forbidden = 6,
  descriptor_invalid = 7,
};

struct SblrDescriptor {
  std::uint16_t kind = 1;
  std::uint16_t flags = 0;
  std::vector<std::uint8_t> payload;
};

struct SblrSourceArtifact {
  std::uint16_t kind = 1;
  std::string value;
};

struct SblrExecutionEnvelope {
  std::uint32_t version_major = 1;
  std::uint32_t version_minor = 0;
  SblrPayloadKind payload_kind = SblrPayloadKind::operation_envelope;
  SblrOperationFamily family = SblrOperationFamily::relational_query;
  std::uint16_t opcode = 1;
  std::uint32_t flags = 0;
  std::vector<SblrDescriptor> descriptors;
  std::vector<SblrSourceArtifact> source_artifacts;
  std::vector<std::uint8_t> canonical_bytes;
};

struct SblrDecodedEnvelope {
  SblrCodecStatus status = SblrCodecStatus::ok;
  SblrExecutionEnvelope envelope;
  std::string_view diagnostic_code;
  std::string_view message_key;
};

struct SblrPriorityDRegistryRow {
  SblrOperationFamily family;
  std::uint16_t opcode_min;
  std::uint16_t opcode_max;
  SblrBehaviorStatus behavior_status;
  std::string_view family_name;
  std::string_view diagnostic_code;
};

inline constexpr std::array<SblrPriorityDRegistryRow, 22> kSblrPriorityDRegistry{{
    {SblrOperationFamily::relational_query, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.relational.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_insert, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.insert.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_update, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.update.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_delete, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.delete.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_merge, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.merge.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::catalog_mutation, 1, 499, SblrBehaviorStatus::admission_only, "sblr.catalog.mutation.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::security_mutation, 1, 499, SblrBehaviorStatus::admission_only, "sblr.security.mutation.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::transaction_control, 1, 499, SblrBehaviorStatus::admission_only,
     "sblr.transaction.control.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::bulk_import, 1, 499, SblrBehaviorStatus::admission_only, "sblr.bulk.import.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::bulk_export, 1, 499, SblrBehaviorStatus::admission_only, "sblr.bulk.export.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::management_inspect, 1, 499, SblrBehaviorStatus::admission_only,
     "sblr.management.inspect.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::management_control, 1, 499, SblrBehaviorStatus::admission_only,
     "sblr.management.control.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::metrics_inspect, 1, 499, SblrBehaviorStatus::admission_only, "sblr.metrics.inspect.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::replication_operation, 1, 499, SblrBehaviorStatus::noncluster_fail_closed,
     "sblr.replication.operation.v3", "SBLR.CAPABILITY.FORBIDDEN"},
    {SblrOperationFamily::structured_kv, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.kv.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::document, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.document.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::graph, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.graph.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::search, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.search.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::vector, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.vector.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::timeseries, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.timeseries.v3",
     "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::versioned_history, 1, 499, SblrBehaviorStatus::admission_only,
     "sblr.versioned_history.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::cluster_placement, 1, 499, SblrBehaviorStatus::noncluster_fail_closed,
     "sblr.cluster.placement.v3", "SBLR.CAPABILITY.FORBIDDEN"},
}};

inline constexpr SblrPriorityDRegistryRow kSblrAccelerationRegistryRow{
    SblrOperationFamily::acceleration_management, 1, 499, SblrBehaviorStatus::capability_fail_closed,
    "sblr.acceleration.management.v3", "SBLR.CAPABILITY.FORBIDDEN"};

inline constexpr SblrPriorityDRegistryRow kSblrDonorMetaRegistryRow{
    SblrOperationFamily::donor_meta, 1, 65535, SblrBehaviorStatus::unsupported, "sblr.donor.meta.forbidden",
    "SBLR.OPCODE.DONOR_META_FORBIDDEN"};

inline constexpr std::uint32_t kSblrEnvelopeMagic = 0x524c4253u;
inline constexpr std::uint32_t kSblrEnvelopeHeaderSize = 32u;
inline constexpr std::uint64_t kSblrMaxEnvelopeBytes = 16ull * 1024ull * 1024ull;

inline const SblrPriorityDRegistryRow* FindSblrPriorityDRegistryRow(SblrOperationFamily family,
                                                                    std::uint16_t opcode) noexcept {
  if (family == kSblrAccelerationRegistryRow.family && opcode >= kSblrAccelerationRegistryRow.opcode_min &&
      opcode <= kSblrAccelerationRegistryRow.opcode_max) {
    return &kSblrAccelerationRegistryRow;
  }
  if (family == kSblrDonorMetaRegistryRow.family) {
    return &kSblrDonorMetaRegistryRow;
  }
  for (const auto& row : kSblrPriorityDRegistry) {
    if (row.family == family && opcode >= row.opcode_min && opcode <= row.opcode_max) {
      return &row;
    }
  }
  return nullptr;
}

inline std::string_view SblrBehaviorStatusName(SblrBehaviorStatus status) noexcept {
  switch (status) {
    case SblrBehaviorStatus::implemented:
      return "implemented";
    case SblrBehaviorStatus::admission_only:
      return "admission_only";
    case SblrBehaviorStatus::noncluster_fail_closed:
      return "noncluster_fail_closed";
    case SblrBehaviorStatus::edition_fail_closed:
      return "edition_fail_closed";
    case SblrBehaviorStatus::capability_fail_closed:
      return "capability_fail_closed";
    case SblrBehaviorStatus::deferred_to_successor:
      return "deferred_to_successor";
    case SblrBehaviorStatus::unsupported:
      return "unsupported";
  }
  return "unsupported";
}

inline void SblrAppendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

inline void SblrAppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
  out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

inline std::uint16_t SblrReadU16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) | (static_cast<std::uint16_t>(data[1]) << 8u);
}

inline std::uint32_t SblrReadU32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8u) |
         (static_cast<std::uint32_t>(data[2]) << 16u) | (static_cast<std::uint32_t>(data[3]) << 24u);
}

inline std::uint32_t SblrChecksum(const std::vector<std::uint8_t>& bytes) {
  std::uint32_t hash = 2166136261u;
  for (std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 16777619u;
  }
  return hash == 0 ? 1u : hash;
}

inline std::vector<std::uint8_t> EncodeSblrEnvelope(const SblrExecutionEnvelope& envelope) {
  std::vector<std::uint8_t> out;
  out.reserve(kSblrEnvelopeHeaderSize + envelope.canonical_bytes.size());
  SblrAppendU32(out, kSblrEnvelopeMagic);
  SblrAppendU16(out, static_cast<std::uint16_t>(envelope.version_major));
  SblrAppendU16(out, static_cast<std::uint16_t>(envelope.version_minor));
  SblrAppendU16(out, static_cast<std::uint16_t>(envelope.payload_kind));
  SblrAppendU16(out, static_cast<std::uint16_t>(envelope.family));
  SblrAppendU16(out, envelope.opcode);
  SblrAppendU16(out, static_cast<std::uint16_t>(envelope.descriptors.size()));
  SblrAppendU32(out, envelope.flags);
  SblrAppendU32(out, static_cast<std::uint32_t>(envelope.canonical_bytes.size()));
  const std::size_t checksum_offset = out.size();
  SblrAppendU32(out, 0);
  SblrAppendU32(out, 0);
  for (const auto& descriptor : envelope.descriptors) {
    SblrAppendU16(out, descriptor.kind);
    SblrAppendU16(out, descriptor.flags);
    SblrAppendU32(out, static_cast<std::uint32_t>(descriptor.payload.size()));
    out.insert(out.end(), descriptor.payload.begin(), descriptor.payload.end());
  }
  out.insert(out.end(), envelope.canonical_bytes.begin(), envelope.canonical_bytes.end());
  const std::uint32_t checksum = SblrChecksum(out);
  out[checksum_offset + 0] = static_cast<std::uint8_t>(checksum & 0xffu);
  out[checksum_offset + 1] = static_cast<std::uint8_t>((checksum >> 8u) & 0xffu);
  out[checksum_offset + 2] = static_cast<std::uint8_t>((checksum >> 16u) & 0xffu);
  out[checksum_offset + 3] = static_cast<std::uint8_t>((checksum >> 24u) & 0xffu);
  return out;
}

inline SblrDecodedEnvelope DecodeSblrEnvelopeBytes(const std::uint8_t* data, std::uint64_t size) {
  SblrDecodedEnvelope decoded;
  if ((size != 0 && data == nullptr) || size < kSblrEnvelopeHeaderSize || size > kSblrMaxEnvelopeBytes) {
    decoded.status = size < kSblrEnvelopeHeaderSize ? SblrCodecStatus::envelope_truncated : SblrCodecStatus::envelope_invalid;
    decoded.diagnostic_code = "SBLR.ENVELOPE.INVALID";
    decoded.message_key = "sblr.envelope.invalid";
    return decoded;
  }
  if (SblrReadU32(data) != kSblrEnvelopeMagic) {
    decoded.status = SblrCodecStatus::envelope_invalid;
    decoded.diagnostic_code = "SBLR.ENVELOPE.INVALID";
    decoded.message_key = "sblr.envelope.invalid";
    return decoded;
  }
  std::vector<std::uint8_t> checksum_bytes(data, data + size);
  const std::uint32_t actual_checksum = SblrReadU32(data + 24);
  checksum_bytes[24] = 0;
  checksum_bytes[25] = 0;
  checksum_bytes[26] = 0;
  checksum_bytes[27] = 0;
  if (actual_checksum != SblrChecksum(checksum_bytes)) {
    decoded.status = SblrCodecStatus::checksum_invalid;
    decoded.diagnostic_code = "SBLR.ENVELOPE.CHECKSUM_INVALID";
    decoded.message_key = "sblr.envelope.checksum_invalid";
    return decoded;
  }
  decoded.envelope.version_major = SblrReadU16(data + 4);
  decoded.envelope.version_minor = SblrReadU16(data + 6);
  if (decoded.envelope.version_major != 1) {
    decoded.status = SblrCodecStatus::version_unsupported;
    decoded.diagnostic_code = "SBLR.VERSION.UNSUPPORTED";
    decoded.message_key = "sblr.version.unsupported";
    return decoded;
  }
  decoded.envelope.payload_kind = static_cast<SblrPayloadKind>(SblrReadU16(data + 8));
  decoded.envelope.family = static_cast<SblrOperationFamily>(SblrReadU16(data + 10));
  decoded.envelope.opcode = SblrReadU16(data + 12);
  const std::uint16_t descriptor_count = SblrReadU16(data + 14);
  decoded.envelope.flags = SblrReadU32(data + 16);
  const std::uint32_t payload_size = SblrReadU32(data + 20);
  const auto* row = FindSblrPriorityDRegistryRow(decoded.envelope.family, decoded.envelope.opcode);
  if (row == nullptr) {
    decoded.status = SblrCodecStatus::opcode_unknown;
    decoded.diagnostic_code = "SBLR.OPCODE.UNKNOWN";
    decoded.message_key = "sblr.opcode.unknown";
    return decoded;
  }
  if (row->family == SblrOperationFamily::donor_meta) {
    decoded.status = SblrCodecStatus::donor_meta_forbidden;
    decoded.diagnostic_code = "SBLR.OPCODE.DONOR_META_FORBIDDEN";
    decoded.message_key = "sblr.opcode.donor_meta_forbidden";
    return decoded;
  }
  std::uint64_t offset = kSblrEnvelopeHeaderSize;
  decoded.envelope.descriptors.reserve(descriptor_count);
  for (std::uint16_t index = 0; index < descriptor_count; ++index) {
    if (offset + 8 > size) {
      decoded.status = SblrCodecStatus::envelope_truncated;
      decoded.diagnostic_code = "SBLR.ENVELOPE.INVALID";
      decoded.message_key = "sblr.envelope.invalid";
      return decoded;
    }
    SblrDescriptor descriptor;
    descriptor.kind = SblrReadU16(data + offset);
    descriptor.flags = SblrReadU16(data + offset + 2);
    const std::uint32_t descriptor_size = SblrReadU32(data + offset + 4);
    offset += 8;
    if (descriptor.kind == 0 || offset + descriptor_size > size) {
      decoded.status = SblrCodecStatus::descriptor_invalid;
      decoded.diagnostic_code = "SBLR.DESCRIPTOR.INVALID";
      decoded.message_key = "sblr.descriptor.invalid";
      return decoded;
    }
    descriptor.payload.assign(data + offset, data + offset + descriptor_size);
    offset += descriptor_size;
    decoded.envelope.descriptors.push_back(std::move(descriptor));
  }
  if (offset + payload_size != size) {
    decoded.status = SblrCodecStatus::envelope_invalid;
    decoded.diagnostic_code = "SBLR.ENVELOPE.INVALID";
    decoded.message_key = "sblr.envelope.invalid";
    return decoded;
  }
  decoded.envelope.canonical_bytes.assign(data + offset, data + offset + payload_size);
  decoded.status = SblrCodecStatus::ok;
  return decoded;
}

}  // namespace scratchbird::engine
