// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
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
  reference_meta = 65000,
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
  reference_meta_forbidden = 6,
  descriptor_invalid = 7,
  field_invalid = 8,
  size_limit_exceeded = 9,
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

struct SblrPriorityDRegistryRow {
  SblrOperationFamily family;
  std::uint16_t opcode_min;
  std::uint16_t opcode_max;
  SblrBehaviorStatus behavior_status;
  std::string_view family_name;
  std::string_view diagnostic_code;
};

inline constexpr std::array<SblrPriorityDRegistryRow, 22> kSblrPriorityDRegistry{{
    {SblrOperationFamily::relational_query, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.relational.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_insert, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.insert.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_update, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.update.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_delete, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.delete.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::dml_merge, 1, 499, SblrBehaviorStatus::admission_only, "sblr.dml.merge.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::catalog_mutation, 1, 499, SblrBehaviorStatus::admission_only, "sblr.catalog.mutation.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::security_mutation, 1, 499, SblrBehaviorStatus::admission_only, "sblr.security.mutation.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::transaction_control, 1, 499, SblrBehaviorStatus::admission_only, "sblr.transaction.control.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::bulk_import, 1, 499, SblrBehaviorStatus::admission_only, "sblr.bulk.import.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::bulk_export, 1, 499, SblrBehaviorStatus::admission_only, "sblr.bulk.export.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::management_inspect, 1, 499, SblrBehaviorStatus::admission_only, "sblr.management.inspect.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::management_control, 1, 499, SblrBehaviorStatus::admission_only, "sblr.management.control.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::metrics_inspect, 1, 499, SblrBehaviorStatus::admission_only, "sblr.metrics.inspect.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::replication_operation, 1, 499, SblrBehaviorStatus::noncluster_fail_closed, "sblr.replication.operation.v3", "SBLR.CAPABILITY.FORBIDDEN"},
    {SblrOperationFamily::structured_kv, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.kv.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::document, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.document.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::graph, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.graph.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::search, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.search.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::vector, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.vector.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::timeseries, 1, 499, SblrBehaviorStatus::admission_only, "sblr.query.timeseries.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::versioned_history, 1, 499, SblrBehaviorStatus::admission_only, "sblr.versioned_history.v3", "SBLR.EXECUTION.ADMISSION_ONLY"},
    {SblrOperationFamily::cluster_placement, 1, 499, SblrBehaviorStatus::noncluster_fail_closed, "sblr.cluster.placement.v3", "SBLR.CAPABILITY.FORBIDDEN"},
}};

inline constexpr SblrPriorityDRegistryRow kSblrAccelerationRegistryRow{
    SblrOperationFamily::acceleration_management, 1, 499,
    SblrBehaviorStatus::capability_fail_closed,
    "sblr.acceleration.management.v3", "SBLR.CAPABILITY.FORBIDDEN"};
inline constexpr SblrPriorityDRegistryRow kSblrReferenceMetaRegistryRow{
    SblrOperationFamily::reference_meta, 1, 65535,
    SblrBehaviorStatus::unsupported,
    "sblr.reference.meta.forbidden", "SBLR.OPCODE.REFERENCE_META_FORBIDDEN"};

inline const SblrPriorityDRegistryRow* FindSblrPriorityDRegistryRow(
    SblrOperationFamily family, std::uint16_t opcode) noexcept {
  if (family == kSblrAccelerationRegistryRow.family && opcode >= 1 && opcode <= 499) {
    return &kSblrAccelerationRegistryRow;
  }
  if (family == SblrOperationFamily::reference_meta) return &kSblrReferenceMetaRegistryRow;
  for (const auto& row : kSblrPriorityDRegistry) {
    if (row.family == family && opcode >= row.opcode_min && opcode <= row.opcode_max) {
      return &row;
    }
  }
  return nullptr;
}

inline std::string_view SblrBehaviorStatusName(SblrBehaviorStatus status) noexcept {
  switch (status) {
    case SblrBehaviorStatus::implemented: return "implemented";
    case SblrBehaviorStatus::admission_only: return "admission_only";
    case SblrBehaviorStatus::noncluster_fail_closed: return "noncluster_fail_closed";
    case SblrBehaviorStatus::edition_fail_closed: return "edition_fail_closed";
    case SblrBehaviorStatus::capability_fail_closed: return "capability_fail_closed";
    case SblrBehaviorStatus::deferred_to_successor: return "deferred_to_successor";
    case SblrBehaviorStatus::unsupported: return "unsupported";
  }
  return "unsupported";
}

inline constexpr std::uint32_t kSblrEnvelopeMagic = 0x524c4253u;
inline constexpr std::uint16_t kSblrContainerMajor = 1;
inline constexpr std::uint16_t kSblrContainerMinor = 1;
inline constexpr std::uint32_t kSblrContainerHeaderSize = 40;
inline constexpr std::uint64_t kSblrMaxEnvelopeBytes = 67'108'864;
inline constexpr std::uint64_t kSblrMaxPayloadBytes = 33'554'432;
inline constexpr std::uint32_t kSblrExecutionEnvelopeMagic = 0x45454253u;
inline constexpr std::uint16_t kSblrExecutionEnvelopeHeaderSize = 48;
inline constexpr std::uint16_t kSblrExecutionEnvelopeFieldCount = 28;

inline void SblrAppendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}
inline void SblrAppendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}
inline void SblrAppendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}
inline std::uint16_t SblrReadU16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(data[1]) << 8;
}
inline std::uint32_t SblrReadU32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         static_cast<std::uint32_t>(data[1]) << 8 |
         static_cast<std::uint32_t>(data[2]) << 16 |
         static_cast<std::uint32_t>(data[3]) << 24;
}
inline std::uint64_t SblrReadU64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift != 64; shift += 8) {
    value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
  }
  return value;
}
inline void SblrStoreU16(std::vector<std::uint8_t>& out, std::size_t offset,
                         std::uint16_t value) {
  out[offset] = static_cast<std::uint8_t>(value);
  out[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}
inline void SblrStoreU32(std::vector<std::uint8_t>& out, std::size_t offset,
                         std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    out[offset + shift / 8] = static_cast<std::uint8_t>(value >> shift);
  }
}
inline void SblrStoreU64(std::vector<std::uint8_t>& out, std::size_t offset,
                         std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out[offset + shift / 8] = static_cast<std::uint8_t>(value >> shift);
  }
}
inline std::uint32_t SblrCrc32c(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

inline bool SblrNonzeroUuid(const std::uint8_t* uuid) {
  for (std::size_t i = 0; i < 16; ++i) if (uuid[i] != 0) return true;
  return false;
}

// SEARCH_KEY: SB_ENGINE_SBLR_CANONICAL_CONTAINER_V1_1
struct SblrCanonicalContainer {
  std::array<std::uint8_t, 132> canonical_anchor{};
  std::vector<std::uint8_t> capability_profile_pin;
  std::vector<std::uint8_t> lowering_metadata;
  std::vector<std::uint8_t> operation_payload;
  std::vector<std::uint8_t> source_map;
  std::vector<std::uint8_t> diagnostic_vector;
};

struct SblrDecodedContainer {
  SblrCodecStatus status = SblrCodecStatus::ok;
  SblrCanonicalContainer container;
  std::string_view diagnostic_code;
  std::string_view message_key;
};

inline std::uint32_t SblrDialectUuidLow32(const std::uint8_t* uuid) {
  return static_cast<std::uint32_t>(uuid[12]) << 24 |
         static_cast<std::uint32_t>(uuid[13]) << 16 |
         static_cast<std::uint32_t>(uuid[14]) << 8 |
         static_cast<std::uint32_t>(uuid[15]);
}

inline bool SblrCanonicalAnchorValid(const std::array<std::uint8_t, 132>& anchor) {
  if (!SblrNonzeroUuid(anchor.data()) || !SblrNonzeroUuid(anchor.data() + 16) ||
      !SblrNonzeroUuid(anchor.data() + 32) || !SblrNonzeroUuid(anchor.data() + 76) ||
      !SblrNonzeroUuid(anchor.data() + 116) || SblrReadU32(anchor.data() + 48) != 1) {
    return false;
  }
  const auto kind = SblrReadU16(anchor.data() + 100);
  if (kind != 1 && kind != 2) return false;
  return std::all_of(anchor.begin() + 102, anchor.begin() + 116,
                     [](std::uint8_t byte) { return byte == 0; });
}

inline void SblrAppendSection(std::vector<std::uint8_t>* out,
                              std::uint32_t tag,
                              const std::vector<std::uint8_t>& payload) {
  SblrAppendU32(*out, tag);
  SblrAppendU32(*out, static_cast<std::uint32_t>(payload.size()));
  out->insert(out->end(), payload.begin(), payload.end());
}

inline std::vector<std::uint8_t> EncodeSblrContainer(
    const SblrCanonicalContainer& container) {
  if (!SblrCanonicalAnchorValid(container.canonical_anchor) ||
      container.operation_payload.empty() ||
      container.operation_payload.size() > kSblrMaxPayloadBytes) {
    return {};
  }
  std::uint32_t flags = 0;
  if (!container.source_map.empty()) flags |= 1u << 2;
  if (!container.capability_profile_pin.empty()) flags |= 1u << 3;
  if (!container.lowering_metadata.empty()) flags |= 1u << 4;
  if (!container.diagnostic_vector.empty()) flags |= 1u << 5;

  std::vector<std::uint8_t> out(kSblrContainerHeaderSize, 0);
  SblrStoreU32(out, 0, kSblrEnvelopeMagic);
  SblrStoreU16(out, 4, kSblrContainerMajor);
  SblrStoreU16(out, 6, kSblrContainerMinor);
  SblrStoreU32(out, 8, flags);
  SblrStoreU64(out, 16, container.operation_payload.size());
  SblrStoreU64(out, 24, container.operation_payload.size());
  SblrStoreU32(out, 32, SblrDialectUuidLow32(container.canonical_anchor.data() + 16));
  const std::vector<std::uint8_t> anchor(container.canonical_anchor.begin(),
                                         container.canonical_anchor.end());
  SblrAppendSection(&out, 0x10, anchor);
  if (!container.capability_profile_pin.empty()) {
    SblrAppendSection(&out, 0x11, container.capability_profile_pin);
  }
  if (!container.lowering_metadata.empty()) {
    SblrAppendSection(&out, 0x12, container.lowering_metadata);
  }
  SblrAppendSection(&out, 0x20, container.operation_payload);
  if (!container.source_map.empty()) SblrAppendSection(&out, 0x30, container.source_map);
  if (!container.diagnostic_vector.empty()) {
    SblrAppendSection(&out, 0x40, container.diagnostic_vector);
  }
  const std::uint32_t crc = SblrCrc32c(out.data(), out.size());
  const std::uint64_t total = out.size() + 20;
  SblrAppendU32(out, 0xfe);
  SblrAppendU32(out, 12);
  SblrAppendU32(out, crc);
  SblrAppendU64(out, total);
  if (out.size() != total || out.size() > kSblrMaxEnvelopeBytes) return {};
  return out;
}

inline SblrDecodedContainer DecodeSblrContainerBytes(const std::uint8_t* data,
                                                      std::uint64_t size) {
  SblrDecodedContainer decoded;
  const auto fail = [&decoded](SblrCodecStatus status,
                               std::string_view code,
                               std::string_view key) {
    decoded.status = status;
    decoded.diagnostic_code = code;
    decoded.message_key = key;
    return decoded;
  };
  if ((size != 0 && data == nullptr) || size < kSblrContainerHeaderSize + 20) {
    return fail(SblrCodecStatus::envelope_truncated,
                "SBLR_CONTAINER.SECTION_MISSING", "sblr.container.section_missing");
  }
  if (size > kSblrMaxEnvelopeBytes) {
    return fail(SblrCodecStatus::size_limit_exceeded,
                "SBLR.ENVELOPE.SIZE_LIMIT_EXCEEDED", "sblr.envelope.size_limit_exceeded");
  }
  if (SblrReadU32(data) != kSblrEnvelopeMagic) {
    return fail(SblrCodecStatus::envelope_invalid,
                "SBLR_CONTAINER.MAGIC_INVALID", "sblr.container.magic_invalid");
  }
  if (SblrReadU16(data + 4) != kSblrContainerMajor ||
      SblrReadU16(data + 6) != kSblrContainerMinor) {
    return fail(SblrCodecStatus::version_unsupported,
                "SBLR_CONTAINER.VERSION_MINOR_UNSUPPORTED", "sblr.container.version_unsupported");
  }
  const std::uint32_t flags = SblrReadU32(data + 8);
  if ((flags & ~0x3fu) != 0 || (flags & 0x03u) != 0 ||
      SblrReadU32(data + 12) != 0 || SblrReadU32(data + 36) != 0) {
    return fail(SblrCodecStatus::envelope_invalid,
                "SBLR_CONTAINER.FLAG_RESERVED_BIT_SET", "sblr.container.flag_invalid");
  }
  const std::uint64_t payload_size = SblrReadU64(data + 16);
  if (payload_size == 0 || payload_size > kSblrMaxPayloadBytes ||
      SblrReadU64(data + 24) != payload_size) {
    return fail(SblrCodecStatus::size_limit_exceeded,
                "SBLR_CONTAINER.PAYLOAD_SIZE_MISMATCH", "sblr.container.payload_size_mismatch");
  }

  const std::array<std::uint32_t, 7> ordered_tags{0x10, 0x11, 0x12, 0x20, 0x30, 0x40, 0xfe};
  std::array<bool, 7> seen{};
  std::size_t offset = kSblrContainerHeaderSize;
  std::size_t order = 0;
  while (offset < size) {
    if (size - offset < 8) {
      return fail(SblrCodecStatus::envelope_truncated,
                  "SBLR_CONTAINER.SECTION_OVERRUN", "sblr.container.section_overrun");
    }
    const std::uint32_t tag = SblrReadU32(data + offset);
    const std::uint32_t section_size = SblrReadU32(data + offset + 4);
    const auto found = std::find(ordered_tags.begin(), ordered_tags.end(), tag);
    if (found == ordered_tags.end()) {
      return fail(SblrCodecStatus::envelope_invalid,
                  "SBLR_CONTAINER.SECTION_TAG_UNKNOWN", "sblr.container.section_tag_unknown");
    }
    const std::size_t index = static_cast<std::size_t>(found - ordered_tags.begin());
    if (seen[index] || index < order) {
      return fail(SblrCodecStatus::envelope_invalid,
                  "SBLR_CONTAINER.SECTION_OUT_OF_ORDER", "sblr.container.section_out_of_order");
    }
    order = index;
    seen[index] = true;
    if (section_size > size - offset - 8) {
      return fail(SblrCodecStatus::envelope_truncated,
                  "SBLR_CONTAINER.SECTION_OVERRUN", "sblr.container.section_overrun");
    }
    const std::uint8_t* section = data + offset + 8;
    if (tag == 0x10) {
      if (section_size != decoded.container.canonical_anchor.size()) {
        return fail(SblrCodecStatus::field_invalid,
                    "SBLR_CONTAINER.SECTION_OVERRUN", "sblr.container.anchor_invalid");
      }
      std::copy(section, section + section_size, decoded.container.canonical_anchor.begin());
    } else if (tag == 0x11) {
      decoded.container.capability_profile_pin.assign(section, section + section_size);
    } else if (tag == 0x12) {
      decoded.container.lowering_metadata.assign(section, section + section_size);
    } else if (tag == 0x20) {
      decoded.container.operation_payload.assign(section, section + section_size);
    } else if (tag == 0x30) {
      decoded.container.source_map.assign(section, section + section_size);
    } else if (tag == 0x40) {
      decoded.container.diagnostic_vector.assign(section, section + section_size);
    } else {
      if (section_size != 12 || offset + 20 != size ||
          SblrReadU64(section + 4) != size) {
        return fail(SblrCodecStatus::envelope_invalid,
                    "SBLR_CONTAINER.TOTAL_SIZE_MISMATCH", "sblr.container.total_size_mismatch");
      }
      if (SblrReadU32(section) != SblrCrc32c(data, offset)) {
        return fail(SblrCodecStatus::checksum_invalid,
                    "SBLR_CONTAINER.CRC_MISMATCH", "sblr.container.crc_mismatch");
      }
    }
    offset += 8 + section_size;
  }
  if (!seen[0] || !seen[3] || !seen[6] ||
      seen[1] != ((flags & (1u << 3)) != 0) ||
      seen[2] != ((flags & (1u << 4)) != 0) ||
      seen[4] != ((flags & (1u << 2)) != 0) ||
      seen[5] != ((flags & (1u << 5)) != 0)) {
    return fail(SblrCodecStatus::field_invalid,
                "SBLR_CONTAINER.SECTION_MISSING", "sblr.container.section_missing");
  }
  if (!SblrCanonicalAnchorValid(decoded.container.canonical_anchor) ||
      SblrDialectUuidLow32(decoded.container.canonical_anchor.data() + 16) !=
          SblrReadU32(data + 32)) {
    return fail(SblrCodecStatus::field_invalid,
                "SBLR_CONTAINER.PAYLOAD_KIND_INVALID", "sblr.container.anchor_invalid");
  }
  if (decoded.container.operation_payload.size() != payload_size) {
    return fail(SblrCodecStatus::field_invalid,
                "SBLR_CONTAINER.PAYLOAD_SIZE_MISMATCH", "sblr.container.payload_size_mismatch");
  }
  const auto canonical = EncodeSblrContainer(decoded.container);
  if (canonical.size() != size || !std::equal(canonical.begin(), canonical.end(), data)) {
    return fail(SblrCodecStatus::field_invalid,
                "SBLR.ENVELOPE.INVALID", "sblr.container.noncanonical");
  }
  decoded.status = SblrCodecStatus::ok;
  return decoded;
}

// The legacy C++ shape remains source-compatible, but its encoder now emits
// only the canonical SBLR v1.1 container. Empty canonical anchors fail closed;
// no 32-byte FNV frame is produced or accepted.
struct SblrExecutionEnvelope {
  std::uint32_t version_major = 1;
  std::uint32_t version_minor = 1;
  SblrPayloadKind payload_kind = SblrPayloadKind::operation_envelope;
  SblrOperationFamily family = SblrOperationFamily::relational_query;
  std::uint16_t opcode = 0;
  std::uint32_t flags = 0;
  std::vector<SblrDescriptor> descriptors;
  std::vector<SblrSourceArtifact> source_artifacts;
  std::array<std::uint8_t, 132> canonical_anchor{};
  std::vector<std::uint8_t> canonical_bytes;
};

struct SblrDecodedEnvelope {
  SblrCodecStatus status = SblrCodecStatus::ok;
  SblrExecutionEnvelope envelope;
  std::string_view diagnostic_code;
  std::string_view message_key;
};

inline std::vector<std::uint8_t> EncodeSblrEnvelope(const SblrExecutionEnvelope& envelope) {
  SblrCanonicalContainer container;
  container.canonical_anchor = envelope.canonical_anchor;
  container.operation_payload = envelope.canonical_bytes;
  return EncodeSblrContainer(container);
}

inline SblrDecodedEnvelope DecodeSblrEnvelopeBytes(const std::uint8_t* data,
                                                    std::uint64_t size) {
  const auto container = DecodeSblrContainerBytes(data, size);
  SblrDecodedEnvelope decoded;
  decoded.status = container.status;
  decoded.diagnostic_code = container.diagnostic_code;
  decoded.message_key = container.message_key;
  if (container.status == SblrCodecStatus::ok) {
    decoded.envelope.version_major = kSblrContainerMajor;
    decoded.envelope.version_minor = kSblrContainerMinor;
    decoded.envelope.canonical_anchor = container.container.canonical_anchor;
    decoded.envelope.payload_kind = static_cast<SblrPayloadKind>(
        SblrReadU16(container.container.canonical_anchor.data() + 100));
    decoded.envelope.canonical_bytes = container.container.operation_payload;
  }
  return decoded;
}

class SblrFieldReader {
 public:
  SblrFieldReader(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size) {}
  bool Take(std::size_t count, const std::uint8_t** value = nullptr) {
    if (count > size_ - offset_) return false;
    if (value != nullptr) *value = data_ + offset_;
    offset_ += count;
    return true;
  }
  bool U8(std::uint8_t* value = nullptr) {
    const std::uint8_t* data = nullptr;
    if (!Take(1, &data)) return false;
    if (value != nullptr) *value = data[0];
    return true;
  }
  bool U16(std::uint16_t* value = nullptr) {
    const std::uint8_t* data = nullptr;
    if (!Take(2, &data)) return false;
    if (value != nullptr) *value = SblrReadU16(data);
    return true;
  }
  bool U32(std::uint32_t* value = nullptr) {
    const std::uint8_t* data = nullptr;
    if (!Take(4, &data)) return false;
    if (value != nullptr) *value = SblrReadU32(data);
    return true;
  }
  bool U64(std::uint64_t* value = nullptr) {
    const std::uint8_t* data = nullptr;
    if (!Take(8, &data)) return false;
    if (value != nullptr) *value = SblrReadU64(data);
    return true;
  }
  std::size_t offset() const { return offset_; }
  std::size_t remaining() const { return size_ - offset_; }
 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
};

inline bool SblrConsumeUuid(SblrFieldReader* reader, bool optional_zero = false) {
  const std::uint8_t* uuid = nullptr;
  if (!reader->Take(16, &uuid)) return false;
  return optional_zero || SblrNonzeroUuid(uuid);
}
inline bool SblrConsumeBytes(SblrFieldReader* reader, std::uint64_t maximum) {
  std::uint64_t size = 0;
  return reader->U64(&size) && size <= maximum && size <= reader->remaining() &&
         reader->Take(static_cast<std::size_t>(size));
}
inline bool SblrConsumeCanonicalStruct(SblrFieldReader* reader,
                                       std::uint64_t maximum) {
  std::uint32_t format = 0;
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint64_t size = 0;
  return reader->U32(&format) && reader->U16(&major) && reader->U16(&minor) &&
         reader->U64(&size) && format != 0 && major != 0 &&
         size != 0 && size <= maximum && size <= reader->remaining() &&
         reader->Take(static_cast<std::size_t>(size));
}
inline bool SblrConsumeReference(SblrFieldReader* reader,
                                 std::uint8_t* kind_out = nullptr,
                                 const std::uint8_t** inline_data = nullptr,
                                 std::uint64_t* inline_size = nullptr) {
  std::uint8_t kind = 0;
  if (!reader->U8(&kind)) return false;
  if (kind_out != nullptr) *kind_out = kind;
  if (kind == 0) return true;
  if (kind == 1) {
    std::uint64_t size = 0;
    if (!reader->U64(&size) || size > kSblrMaxPayloadBytes ||
        size > reader->remaining()) return false;
    const std::uint8_t* data = nullptr;
    if (!reader->Take(static_cast<std::size_t>(size), &data)) return false;
    if (inline_data != nullptr) *inline_data = data;
    if (inline_size != nullptr) *inline_size = size;
    return true;
  }
  return (kind == 2 || kind == 3 || kind == 4) &&
         SblrConsumeUuid(reader) && reader->U64() && reader->U32();
}
inline bool SblrConsumeChecksum(SblrFieldReader* reader,
                                std::uint8_t* kind_out = nullptr,
                                std::uint32_t* crc_out = nullptr) {
  std::uint8_t kind = 0;
  if (!reader->U8(&kind)) return false;
  if (kind_out != nullptr) *kind_out = kind;
  if (kind == 0) return true;
  if (kind == 1) return reader->U32(crc_out);
  return kind == 2 && reader->Take(32);
}

// SEARCH_KEY: SB_ENGINE_SBLR_EXECUTION_ENVELOPE_V1
struct SblrExecutionEnvelopeV1 {
  std::uint32_t header_flags = 0;
  std::array<std::vector<std::uint8_t>, kSblrExecutionEnvelopeFieldCount> fields;
};

struct SblrDecodedExecutionEnvelopeV1 {
  SblrCodecStatus status = SblrCodecStatus::ok;
  SblrExecutionEnvelopeV1 envelope;
  std::string_view diagnostic_code;
  std::string_view message_key;
};

struct SblrExecutionEnvelopeSemanticView {
  SblrPayloadKind payload_kind = SblrPayloadKind::operation_envelope;
  std::uint8_t opcode_ref_kind = 0;
  std::uint8_t operation_ref_kind = 0;
  const std::uint8_t* operation_inline_data = nullptr;
  std::uint64_t operation_inline_size = 0;
  std::uint64_t payload_size = 0;
  std::uint8_t payload_checksum_kind = 0;
  std::uint32_t payload_crc32c = 0;
  std::uint16_t submission_source = 0;
  bool dialect_present = false;
  bool user_present = false;
  bool diagnostic_context_present = false;
  bool source_artifact_present = false;
  std::uint8_t source_artifact_ref_kind = 0;
  bool cluster_context_present = false;
};

inline bool SblrValidateExecutionEnvelopeFields(
    const SblrExecutionEnvelopeV1& envelope,
    SblrExecutionEnvelopeSemanticView* semantic = nullptr) {
  if ((envelope.header_flags & ~0x0fu) != 0) return false;
  SblrExecutionEnvelopeSemanticView view;
  for (std::size_t ordinal = 0; ordinal < envelope.fields.size(); ++ordinal) {
    const auto& field = envelope.fields[ordinal];
    if (field.empty()) return false;
    SblrFieldReader reader(field.data(), field.size());
    bool ok = false;
    switch (ordinal + 1) {
      case 1: {
        const std::uint8_t* uuid = nullptr;
        ok = reader.Take(16, &uuid) && SblrNonzeroUuid(uuid) &&
             (uuid[6] >> 4) == 7;
        break;
      }
      case 2: {
        std::uint16_t value = 0;
        ok = reader.U16(&value) && value == 1;
        break;
      }
      case 3: {
        std::uint16_t value = 0;
        ok = reader.U16(&value) && value == 0;
        break;
      }
      case 4: {
        std::uint32_t value = 0;
        ok = reader.U32(&value) && value != 0;
        break;
      }
      case 5: {
        std::uint16_t value = 0;
        ok = reader.U16(&value) && (value == 1 || value == 2);
        view.payload_kind = static_cast<SblrPayloadKind>(value);
        break;
      }
      case 6:
        ok = SblrConsumeReference(&reader, &view.opcode_ref_kind);
        break;
      case 7:
        ok = SblrConsumeReference(&reader, &view.operation_ref_kind,
                                  &view.operation_inline_data,
                                  &view.operation_inline_size);
        break;
      case 8:
        ok = SblrConsumeChecksum(&reader, &view.payload_checksum_kind,
                                 &view.payload_crc32c);
        break;
      case 9:
        ok = reader.U64(&view.payload_size) && view.payload_size <= kSblrMaxPayloadBytes;
        break;
      case 10:
        ok = reader.U16(&view.submission_source) &&
             view.submission_source >= 1 && view.submission_source <= 6;
        break;
      case 11:
      case 12: {
        std::uint8_t present = 0;
        ok = reader.U8(&present) && present <= 1 &&
             (present == 0 || SblrConsumeUuid(&reader));
        if (ordinal == 10) view.dialect_present = present == 1;
        else view.user_present = present == 1;
        break;
      }
      case 13:
      case 14:
      case 22:
        ok = SblrConsumeCanonicalStruct(&reader, kSblrMaxEnvelopeBytes);
        break;
      case 15:
      case 20:
      case 28: {
        std::uint8_t present = 0;
        ok = reader.U8(&present) && present <= 1 &&
             (present == 0 || SblrConsumeCanonicalStruct(
                 &reader, ordinal == 19 ? 8'388'608 : kSblrMaxEnvelopeBytes));
        if (ordinal == 27) view.cluster_context_present = present == 1;
        break;
      }
      case 16:
        ok = reader.U64();
        break;
      case 17:
      case 21: {
        std::uint32_t count = 0;
        const std::uint32_t maximum = ordinal == 16 ? 262'144 : 4'096;
        ok = reader.U32(&count) && count <= maximum;
        for (std::uint32_t i = 0; ok && i < count; ++i) ok = SblrConsumeUuid(&reader);
        break;
      }
      case 18:
      case 19: {
        std::uint32_t count = 0;
        const std::uint32_t maximum = ordinal == 17 ? 131'072 : 262'144;
        ok = reader.U32(&count) && count <= maximum;
        for (std::uint32_t i = 0; ok && i < count; ++i) {
          ok = SblrConsumeCanonicalStruct(&reader, kSblrMaxEnvelopeBytes);
        }
        break;
      }
      case 23:
      case 24: {
        std::uint8_t present = 0;
        ok = reader.U8(&present) && present <= 1;
        std::uint8_t kind = 0;
        if (ok && present == 1) ok = SblrConsumeReference(&reader, &kind);
        if (ordinal == 22) view.diagnostic_context_present = present == 1;
        else {
          view.source_artifact_present = present == 1;
          view.source_artifact_ref_kind = kind;
        }
        break;
      }
      case 25:
        ok = SblrConsumeChecksum(&reader);
        break;
      case 26: {
        std::uint16_t value = 0;
        ok = reader.U16(&value) && value <= 4;
        break;
      }
      case 27: {
        std::uint8_t present = 0;
        ok = reader.U8(&present) && present <= 1 &&
             (present == 0 || SblrConsumeBytes(&reader, 65'536));
        break;
      }
    }
    if (!ok || reader.remaining() != 0) return false;
  }
  if ((view.payload_kind == SblrPayloadKind::operation_envelope &&
       (view.opcode_ref_kind != 0 || view.operation_ref_kind == 0)) ||
      (view.payload_kind == SblrPayloadKind::opcode_stream &&
       (view.operation_ref_kind != 0 || view.opcode_ref_kind == 0)) ||
      view.payload_checksum_kind == 0 || view.payload_size == 0 ||
      (view.submission_source == 1 && !view.dialect_present) ||
      ((envelope.header_flags & (1u << 1)) == 0 && !view.user_present) ||
      view.diagnostic_context_present != ((envelope.header_flags & (1u << 2)) != 0) ||
      view.cluster_context_present != ((envelope.header_flags & (1u << 3)) != 0) ||
      ((envelope.header_flags & 1u) != 0 &&
       (!view.source_artifact_present || view.source_artifact_ref_kind != 4)) ||
      ((envelope.header_flags & 1u) == 0 && view.source_artifact_ref_kind == 4)) {
    return false;
  }
  if (view.operation_ref_kind == 1) {
    if (view.operation_inline_size != view.payload_size ||
        view.payload_checksum_kind != 1 ||
        SblrCrc32c(view.operation_inline_data,
                   static_cast<std::size_t>(view.operation_inline_size)) !=
            view.payload_crc32c) return false;
  }
  if (semantic != nullptr) *semantic = view;
  return true;
}

inline std::vector<std::uint8_t> EncodeSblrExecutionEnvelopeV1(
    const SblrExecutionEnvelopeV1& envelope) {
  if (!SblrValidateExecutionEnvelopeFields(envelope)) return {};
  std::uint64_t field_size = 0;
  for (const auto& field : envelope.fields) {
    if (field.size() > kSblrMaxEnvelopeBytes - field_size) return {};
    field_size += field.size();
  }
  if (field_size == 0 || field_size + kSblrExecutionEnvelopeHeaderSize >
                             kSblrMaxEnvelopeBytes) return {};
  std::vector<std::uint8_t> out(kSblrExecutionEnvelopeHeaderSize, 0);
  SblrStoreU32(out, 0, kSblrExecutionEnvelopeMagic);
  SblrStoreU16(out, 4, 1);
  SblrStoreU16(out, 6, 0);
  SblrStoreU16(out, 8, kSblrExecutionEnvelopeHeaderSize);
  SblrStoreU16(out, 10, kSblrExecutionEnvelopeFieldCount);
  SblrStoreU32(out, 12, envelope.header_flags);
  SblrStoreU64(out, 16, field_size);
  SblrStoreU64(out, 32, field_size + kSblrExecutionEnvelopeHeaderSize);
  for (const auto& field : envelope.fields) {
    out.insert(out.end(), field.begin(), field.end());
  }
  SblrStoreU32(out, 24, SblrCrc32c(out.data() + kSblrExecutionEnvelopeHeaderSize,
                                   static_cast<std::size_t>(field_size)));
  return out;
}

inline bool SblrConsumeExecutionField(std::size_t ordinal,
                                      SblrFieldReader* reader) {
  switch (ordinal) {
    case 1: return SblrConsumeUuid(reader);
    case 2:
    case 3:
    case 5:
    case 10:
    case 26: return reader->U16();
    case 4: return reader->U32();
    case 6:
    case 7: return SblrConsumeReference(reader);
    case 8:
    case 25: return SblrConsumeChecksum(reader);
    case 9:
    case 16: return reader->U64();
    case 11:
    case 12: {
      std::uint8_t present = 0;
      return reader->U8(&present) && present <= 1 &&
             (present == 0 || SblrConsumeUuid(reader));
    }
    case 13:
    case 14:
    case 22: return SblrConsumeCanonicalStruct(reader, kSblrMaxEnvelopeBytes);
    case 15:
    case 20:
    case 28: {
      std::uint8_t present = 0;
      return reader->U8(&present) && present <= 1 &&
             (present == 0 || SblrConsumeCanonicalStruct(reader, kSblrMaxEnvelopeBytes));
    }
    case 17:
    case 21: {
      std::uint32_t count = 0;
      const std::uint32_t maximum = ordinal == 17 ? 262'144 : 4'096;
      if (!reader->U32(&count) || count > maximum) return false;
      for (std::uint32_t i = 0; i < count; ++i) if (!SblrConsumeUuid(reader)) return false;
      return true;
    }
    case 18:
    case 19: {
      std::uint32_t count = 0;
      const std::uint32_t maximum = ordinal == 18 ? 131'072 : 262'144;
      if (!reader->U32(&count) || count > maximum) return false;
      for (std::uint32_t i = 0; i < count; ++i) {
        if (!SblrConsumeCanonicalStruct(reader, kSblrMaxEnvelopeBytes)) return false;
      }
      return true;
    }
    case 23:
    case 24: {
      std::uint8_t present = 0;
      return reader->U8(&present) && present <= 1 &&
             (present == 0 || SblrConsumeReference(reader));
    }
    case 27: {
      std::uint8_t present = 0;
      return reader->U8(&present) && present <= 1 &&
             (present == 0 || SblrConsumeBytes(reader, 65'536));
    }
  }
  return false;
}

inline SblrDecodedExecutionEnvelopeV1 DecodeSblrExecutionEnvelopeV1Bytes(
    const std::uint8_t* data, std::uint64_t size) {
  SblrDecodedExecutionEnvelopeV1 decoded;
  const auto fail = [&decoded](SblrCodecStatus status,
                               std::string_view code,
                               std::string_view key) {
    decoded.status = status;
    decoded.diagnostic_code = code;
    decoded.message_key = key;
    return decoded;
  };
  if ((size != 0 && data == nullptr) || size < kSblrExecutionEnvelopeHeaderSize) {
    return fail(SblrCodecStatus::envelope_truncated,
                "SBLR.ENVELOPE.INVALID", "sblr.envelope.invalid");
  }
  if (size > kSblrMaxEnvelopeBytes) {
    return fail(SblrCodecStatus::size_limit_exceeded,
                "SBLR.ENVELOPE.SIZE_LIMIT_EXCEEDED", "sblr.envelope.size_limit_exceeded");
  }
  if (SblrReadU32(data) != kSblrExecutionEnvelopeMagic) {
    return fail(SblrCodecStatus::envelope_invalid,
                "SBLR.ENVELOPE.INVALID", "sblr.envelope.magic_invalid");
  }
  if (SblrReadU16(data + 4) != 1 || SblrReadU16(data + 6) != 0) {
    return fail(SblrCodecStatus::version_unsupported,
                "SBLR.ENVELOPE.VERSION_INVALID", "sblr.envelope.version_invalid");
  }
  if (SblrReadU16(data + 8) != kSblrExecutionEnvelopeHeaderSize ||
      SblrReadU16(data + 10) != kSblrExecutionEnvelopeFieldCount ||
      (SblrReadU32(data + 12) & ~0x0fu) != 0 || SblrReadU32(data + 28) != 0 ||
      SblrReadU64(data + 40) != 0) {
    return fail(SblrCodecStatus::envelope_invalid,
                "SBLR.ENVELOPE.FIELD_COUNT_INVALID", "sblr.envelope.header_invalid");
  }
  const std::uint64_t field_size = SblrReadU64(data + 16);
  if (field_size == 0 || field_size != size - kSblrExecutionEnvelopeHeaderSize ||
      SblrReadU64(data + 32) != size) {
    return fail(SblrCodecStatus::envelope_invalid,
                "SBLR.ENVELOPE.INVALID", "sblr.envelope.total_size_mismatch");
  }
  if (SblrReadU32(data + 24) !=
      SblrCrc32c(data + kSblrExecutionEnvelopeHeaderSize,
                 static_cast<std::size_t>(field_size))) {
    return fail(SblrCodecStatus::checksum_invalid,
                "SBLR.ENVELOPE.CHECKSUM_MISMATCH", "sblr.envelope.checksum_mismatch");
  }
  decoded.envelope.header_flags = SblrReadU32(data + 12);
  SblrFieldReader reader(data + kSblrExecutionEnvelopeHeaderSize,
                         static_cast<std::size_t>(field_size));
  for (std::size_t ordinal = 1; ordinal <= kSblrExecutionEnvelopeFieldCount; ++ordinal) {
    const std::size_t begin = reader.offset();
    if (!SblrConsumeExecutionField(ordinal, &reader)) {
      return fail(SblrCodecStatus::field_invalid,
                  "SBLR.ENVELOPE.FIELD_MISSING", "sblr.envelope.field_invalid");
    }
    decoded.envelope.fields[ordinal - 1].assign(
        data + kSblrExecutionEnvelopeHeaderSize + begin,
        data + kSblrExecutionEnvelopeHeaderSize + reader.offset());
  }
  if (reader.remaining() != 0 ||
      !SblrValidateExecutionEnvelopeFields(decoded.envelope)) {
    return fail(SblrCodecStatus::field_invalid,
                "SBLR.ENVELOPE.FIELD_UNEXPECTED", "sblr.envelope.field_unexpected");
  }
  const auto canonical = EncodeSblrExecutionEnvelopeV1(decoded.envelope);
  if (canonical.size() != size || !std::equal(canonical.begin(), canonical.end(), data)) {
    return fail(SblrCodecStatus::field_invalid,
                "SBLR.ENVELOPE.INVALID", "sblr.envelope.noncanonical");
  }
  decoded.status = SblrCodecStatus::ok;
  return decoded;
}

}  // namespace scratchbird::engine
