// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_local_private_relation_locator.hpp"

#include "hash_digest.hpp"
#include "local_transaction_store.hpp"
#include "page_manager.hpp"
#include "row_data_page.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::storage::database {
namespace {

namespace core_hash = scratchbird::core::hash;
namespace core_uuid = scratchbird::core::uuid;
namespace disk = scratchbird::storage::disk;
namespace mga = scratchbird::transaction::mga;
namespace page = scratchbird::storage::page;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

constexpr std::array<byte, 8> kMarkerMagic = {
    'S', 'B', 'S', 'L', 'C', 'M', '0', '1'};
constexpr std::array<byte, 8> kAnchorMagic = {
    'S', 'B', 'S', 'L', 'C', 'A', '0', '1'};
constexpr std::array<byte, 8> kLocatorMagic = {
    'S', 'B', 'S', 'L', 'C', 'R', '0', '1'};
constexpr u16 kFormatMajor = 1;
constexpr u16 kFormatMinor = 0;
constexpr u32 kMarkerFreshFlags = 3;
constexpr u32 kMarkerMigratedFlags = 5;
constexpr u32 kAnchorPublishedFlags = 1;
constexpr std::string_view kMarkerHashDomain =
    "ScratchBird.DatabaseLocalSecurity.Marker.V1";
constexpr std::string_view kAnchorHashDomain =
    "ScratchBird.DatabaseLocalSecurity.Anchor.V1";
constexpr std::string_view kLocatorHashDomain =
    "ScratchBird.DatabaseLocalSecurity.Locator.V1";
constexpr std::array<byte, 8> kSecurityBatchMagic = {
    'S', 'B', 'S', 'E', 'D', 'B', '0', '1'};
constexpr u16 kSecurityBatchMajor = 1;
constexpr u16 kLegacySecurityBatchMinor = 0;
constexpr u16 kCurrentSecurityBatchMinor = 1;
constexpr std::size_t kLegacySecurityBatchFixedBytes = 140;
constexpr std::size_t kCurrentSecurityBatchFixedBytes =
    kDatabaseLocalSecurityBatchEnvelopeBytesV1;
constexpr std::string_view kLifecycleMagic = "SBSECPL1";
constexpr std::string_view kSuccessorKind = "AUTH_CONTEXT_SUCCESSOR";

Status LocatorOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status LocatorErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::storage_page};
}

template <typename Result>
Result Refuse(std::string diagnostic_code,
              std::string message_key,
              std::string detail = {}) {
  Result result;
  result.status = LocatorErrorStatus();
  result.diagnostic = MakeDatabaseLocalPrivateSecurityLocatorDiagnosticV1(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

template <typename Result>
Result Propagate(Status status, DiagnosticRecord diagnostic) {
  Result result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

template <typename Bytes>
bool AllZero(const Bytes& bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](byte value) { return value == 0; });
}

template <typename Bytes>
void StoreU16(Bytes* bytes, std::size_t offset, u16 value) {
  if (bytes == nullptr || offset + 2 > bytes->size()) return;
  scratchbird::core::platform::StoreLittle16(bytes->data() + offset, value);
}

template <typename Bytes>
void StoreU32(Bytes* bytes, std::size_t offset, u32 value) {
  if (bytes == nullptr || offset + 4 > bytes->size()) return;
  scratchbird::core::platform::StoreLittle32(bytes->data() + offset, value);
}

template <typename Bytes>
void StoreU64(Bytes* bytes, std::size_t offset, u64 value) {
  if (bytes == nullptr || offset + 8 > bytes->size()) return;
  scratchbird::core::platform::StoreLittle64(bytes->data() + offset, value);
}

template <typename Bytes>
u16 LoadU16(const Bytes& bytes, std::size_t offset) {
  return offset + 2 <= bytes.size()
             ? scratchbird::core::platform::LoadLittle16(bytes.data() + offset)
             : 0;
}

template <typename Bytes>
u32 LoadU32(const Bytes& bytes, std::size_t offset) {
  return offset + 4 <= bytes.size()
             ? scratchbird::core::platform::LoadLittle32(bytes.data() + offset)
             : 0;
}

template <typename Bytes>
u64 LoadU64(const Bytes& bytes, std::size_t offset) {
  return offset + 8 <= bytes.size()
             ? scratchbird::core::platform::LoadLittle64(bytes.data() + offset)
             : 0;
}

template <typename Bytes>
void StoreUuid(Bytes* bytes, std::size_t offset, const Uuid& value) {
  if (bytes == nullptr || offset + value.bytes.size() > bytes->size()) return;
  std::copy(value.bytes.begin(), value.bytes.end(), bytes->begin() + offset);
}

template <typename Bytes>
Uuid LoadUuid(const Bytes& bytes, std::size_t offset) {
  Uuid value;
  if (offset + value.bytes.size() <= bytes.size()) {
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                value.bytes.size(), value.bytes.begin());
  }
  return value;
}

template <typename Bytes>
void StoreDigest(Bytes* bytes,
                 std::size_t offset,
                 const DatabaseLocalPrivateSecuritySha256V1& value) {
  if (bytes == nullptr || offset + value.size() > bytes->size()) return;
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

template <typename Bytes>
DatabaseLocalPrivateSecuritySha256V1 LoadDigest(const Bytes& bytes,
                                                std::size_t offset) {
  DatabaseLocalPrivateSecuritySha256V1 value{};
  if (offset + value.size() <= bytes.size()) {
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                value.size(), value.begin());
  }
  return value;
}

template <typename Bytes>
DatabaseLocalPrivateSecuritySha256V1 HashRecord(
    std::string_view domain,
    Bytes bytes,
    std::size_t digest_offset) {
  DatabaseLocalPrivateSecuritySha256V1 result{};
  if (digest_offset + result.size() > bytes.size()) return result;
  std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(digest_offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(digest_offset +
                                                       result.size()),
            0);
  std::vector<byte> material;
  material.reserve(domain.size() + bytes.size());
  material.insert(material.end(), domain.begin(), domain.end());
  material.insert(material.end(), bytes.begin(), bytes.end());
  const auto digest = core_hash::ComputeSha256Digest(material);
  if (digest.ok()) result = digest.digest;
  return result;
}

TypedUuid SecurityRelationUuid() {
  const auto parsed = core_uuid::ParseDurableEngineIdentityUuid(
      UuidKind::object, kDatabaseLocalPrivateSecurityRelationUuidV1);
  return parsed.ok() ? parsed.value : TypedUuid{};
}

bool SameUuid(const TypedUuid& typed, const Uuid& raw, UuidKind kind) {
  return typed.kind == kind && typed.valid() && typed.value == raw;
}

bool SameAnchorLogicalFields(
    const DatabaseLocalPrivateSecurityAnchorV1& left,
    const DatabaseLocalPrivateSecurityAnchorV1& right) {
  return left.flags == right.flags && left.database_uuid == right.database_uuid &&
         left.relation_uuid == right.relation_uuid &&
         left.relation_generation == right.relation_generation &&
         left.anchor_generation == right.anchor_generation &&
         left.locator_slot == right.locator_slot &&
         left.locator_generation == right.locator_generation &&
         left.locator_sha256 == right.locator_sha256;
}

struct DecodedAnchor {
  bool shape_valid = false;
  bool digest_valid = false;
  DatabaseLocalPrivateSecurityAnchorV1 value;
};

struct DecodedLocator {
  bool zero = true;
  bool shape_valid = false;
  bool digest_valid = false;
  DatabaseLocalPrivateSecurityLocatorV1 value;
};

bool DecodeMarker(const SerializedDatabaseLocalPrivateSecurityMarkerV1& bytes,
                  DatabaseLocalPrivateSecurityMarkerV1* marker,
                  std::string* refusal) {
  auto fail = [&](std::string detail) {
    if (refusal != nullptr) *refusal = std::move(detail);
    return false;
  };
  if (marker == nullptr ||
      !std::equal(kMarkerMagic.begin(), kMarkerMagic.end(), bytes.begin()) ||
      LoadU16(bytes, 8) != kFormatMajor ||
      LoadU16(bytes, 10) != kFormatMinor ||
      LoadU32(bytes, 12) != kDatabaseLocalPrivateSecurityMarkerBytesV1 ||
      LoadU32(bytes, 20) != 0) {
    return fail("marker_header_invalid");
  }
  marker->flags = LoadU32(bytes, 16);
  marker->database_uuid = LoadUuid(bytes, 24);
  marker->relation_uuid = LoadUuid(bytes, 40);
  marker->relation_generation = LoadU64(bytes, 56);
  marker->migration_scan_count = LoadU64(bytes, 64);
  marker->locator_page_uuid = LoadUuid(bytes, 72);
  marker->locator_page_number = LoadU64(bytes, 88);
  marker->locator_page_generation = LoadU64(bytes, 96);
  marker->initial_locator_generation = LoadU64(bytes, 104);
  marker->initial_locator_sha256 = LoadDigest(bytes, 112);
  marker->marker_sha256 = LoadDigest(bytes, 144);
  const auto expected = HashRecord(kMarkerHashDomain, bytes, 144);
  const TypedUuid relation_uuid = SecurityRelationUuid();
  if (!relation_uuid.valid() || marker->relation_uuid != relation_uuid.value ||
      marker->relation_generation !=
          kDatabaseLocalPrivateSecurityRelationGenerationV1 ||
      marker->locator_page_number !=
          kDatabaseLocalPrivateSecurityRootPageNumberV1 ||
      marker->locator_page_uuid.is_nil() ||
      marker->locator_page_generation == 0 ||
      marker->initial_locator_generation != 1 ||
      AllZero(marker->initial_locator_sha256) ||
      marker->marker_sha256 != expected) {
    return fail("marker_identity_or_digest_invalid");
  }
  if ((marker->flags == kMarkerFreshFlags &&
       marker->migration_scan_count != 0) ||
      (marker->flags == kMarkerMigratedFlags &&
       marker->migration_scan_count != 1) ||
      (marker->flags != kMarkerFreshFlags &&
       marker->flags != kMarkerMigratedFlags)) {
    return fail("marker_flags_or_scan_count_invalid");
  }
  return true;
}

DecodedAnchor DecodeAnchor(
    const SerializedDatabaseLocalPrivateSecurityAnchorV1& bytes,
    u32 expected_ordinal) {
  DecodedAnchor decoded;
  auto& anchor = decoded.value;
  anchor.copy_ordinal = LoadU32(bytes, 16);
  anchor.flags = LoadU32(bytes, 20);
  anchor.database_uuid = LoadUuid(bytes, 24);
  anchor.relation_uuid = LoadUuid(bytes, 40);
  anchor.relation_generation = LoadU64(bytes, 56);
  anchor.anchor_generation = LoadU64(bytes, 64);
  anchor.locator_slot = LoadU32(bytes, 72);
  anchor.locator_generation = LoadU64(bytes, 80);
  anchor.locator_sha256 = LoadDigest(bytes, 88);
  anchor.anchor_sha256 = LoadDigest(bytes, 120);
  decoded.shape_valid =
      std::equal(kAnchorMagic.begin(), kAnchorMagic.end(), bytes.begin()) &&
      LoadU16(bytes, 8) == kFormatMajor &&
      LoadU16(bytes, 10) == kFormatMinor &&
      LoadU32(bytes, 12) == kDatabaseLocalPrivateSecurityAnchorBytesV1 &&
      anchor.copy_ordinal == expected_ordinal &&
      anchor.flags == kAnchorPublishedFlags && LoadU32(bytes, 76) == 0 &&
      anchor.relation_generation ==
          kDatabaseLocalPrivateSecurityRelationGenerationV1 &&
      anchor.anchor_generation != 0 && anchor.locator_slot < 2 &&
      anchor.locator_generation != 0 && !AllZero(anchor.locator_sha256);
  decoded.digest_valid =
      decoded.shape_valid &&
      anchor.anchor_sha256 == HashRecord(kAnchorHashDomain, bytes, 120);
  return decoded;
}

DecodedLocator DecodeLocator(
    const SerializedDatabaseLocalPrivateSecurityLocatorV1& bytes,
    u32 expected_ordinal) {
  DecodedLocator decoded;
  decoded.zero = AllZero(bytes);
  if (decoded.zero) return decoded;
  auto& locator = decoded.value;
  locator.slot_ordinal = LoadU32(bytes, 16);
  locator.lineage = static_cast<DatabaseLocalPrivateSecurityLocatorLineageV1>(
      LoadU32(bytes, 20));
  locator.database_uuid = LoadUuid(bytes, 24);
  locator.filespace_uuid = LoadUuid(bytes, 40);
  locator.relation_uuid = LoadUuid(bytes, 56);
  locator.relation_generation = LoadU64(bytes, 72);
  locator.locator_generation = LoadU64(bytes, 80);
  locator.creator_transaction_uuid = LoadUuid(bytes, 88);
  locator.creator_local_transaction_id = LoadU64(bytes, 104);
  locator.head_page_uuid = LoadUuid(bytes, 112);
  locator.head_page_number = LoadU64(bytes, 128);
  locator.head_page_generation = LoadU64(bytes, 136);
  locator.tail_page_uuid = LoadUuid(bytes, 144);
  locator.tail_page_number = LoadU64(bytes, 160);
  locator.tail_page_generation = LoadU64(bytes, 168);
  locator.chain_page_count = LoadU64(bytes, 176);
  locator.extent_min_page = LoadU64(bytes, 184);
  locator.extent_max_page = LoadU64(bytes, 192);
  locator.security_context_generation = LoadU64(bytes, 200);
  locator.prior_locator_sha256 = LoadDigest(bytes, 208);
  locator.record_sha256 = LoadDigest(bytes, 240);
  const bool known_lineage =
      locator.lineage ==
          DatabaseLocalPrivateSecurityLocatorLineageV1::fresh_bootstrap ||
      locator.lineage ==
          DatabaseLocalPrivateSecurityLocatorLineageV1::sealed_legacy_migration;
  const bool empty = locator.chain_page_count == 0;
  const bool empty_shape =
      locator.locator_generation == 1 &&
      locator.creator_transaction_uuid.is_nil() &&
      locator.creator_local_transaction_id == 0 &&
      locator.head_page_uuid.is_nil() && locator.head_page_number == 0 &&
      locator.head_page_generation == 0 && locator.tail_page_uuid.is_nil() &&
      locator.tail_page_number == 0 && locator.tail_page_generation == 0 &&
      locator.extent_min_page == 0 && locator.extent_max_page == 0 &&
      AllZero(locator.prior_locator_sha256);
  const bool migrated_baseline =
      locator.locator_generation == 1 &&
      locator.lineage ==
          DatabaseLocalPrivateSecurityLocatorLineageV1::sealed_legacy_migration &&
      locator.creator_transaction_uuid.is_nil() &&
      locator.creator_local_transaction_id == 0 &&
      AllZero(locator.prior_locator_sha256);
  const bool successor =
      locator.locator_generation > 1 &&
      !locator.creator_transaction_uuid.is_nil() &&
      locator.creator_local_transaction_id != 0 &&
      !AllZero(locator.prior_locator_sha256);
  const bool nonempty_shape =
      (migrated_baseline || successor) && !locator.head_page_uuid.is_nil() &&
      locator.head_page_number >=
          kDatabaseLocalPrivateSecurityFirstDataPageNumberV1 &&
      locator.head_page_generation != 0 && !locator.tail_page_uuid.is_nil() &&
      locator.tail_page_number >=
          kDatabaseLocalPrivateSecurityFirstDataPageNumberV1 &&
      locator.tail_page_generation != 0 &&
      locator.extent_min_page >=
          kDatabaseLocalPrivateSecurityFirstDataPageNumberV1 &&
      locator.extent_min_page <= locator.extent_max_page &&
      locator.head_page_number >= locator.extent_min_page &&
      locator.head_page_number <= locator.extent_max_page &&
      locator.tail_page_number >= locator.extent_min_page &&
      locator.tail_page_number <= locator.extent_max_page;
  decoded.shape_valid =
      std::equal(kLocatorMagic.begin(), kLocatorMagic.end(), bytes.begin()) &&
      LoadU16(bytes, 8) == kFormatMajor &&
      LoadU16(bytes, 10) == kFormatMinor &&
      LoadU32(bytes, 12) == kDatabaseLocalPrivateSecurityLocatorBytesV1 &&
      locator.slot_ordinal == expected_ordinal && known_lineage &&
      locator.relation_generation ==
          kDatabaseLocalPrivateSecurityRelationGenerationV1 &&
      locator.locator_generation != 0 &&
      locator.security_context_generation != 0 &&
      ((empty && empty_shape) || (!empty && nonempty_shape));
  decoded.digest_valid =
      decoded.shape_valid &&
      locator.record_sha256 == HashRecord(kLocatorHashDomain, bytes, 240);
  return decoded;
}

template <std::size_t N>
std::array<byte, N> Slice(const std::vector<byte>& bytes,
                          std::size_t offset) {
  std::array<byte, N> result{};
  if (offset <= bytes.size() && N <= bytes.size() - offset) {
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), N,
                result.begin());
  }
  return result;
}

bool BodyHasOnlyAssignedBytes(const std::vector<byte>& body) {
  if (body.size() < 880) return false;
  std::array<std::pair<std::size_t, std::size_t>, 4> assigned = {{
      {kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[0],
       kDatabaseLocalPrivateSecurityAnchorBytesV1},
      {kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[1],
       kDatabaseLocalPrivateSecurityAnchorBytesV1},
      {kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[0],
       kDatabaseLocalPrivateSecurityLocatorBytesV1},
      {kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[1],
       kDatabaseLocalPrivateSecurityLocatorBytesV1},
  }};
  for (std::size_t offset = 0; offset < body.size(); ++offset) {
    bool is_assigned = false;
    for (const auto& range : assigned) {
      if (offset >= range.first && offset < range.first + range.second) {
        is_assigned = true;
        break;
      }
    }
    if (!is_assigned && body[offset] != 0) return false;
  }
  return true;
}

bool LocatorMatchesAnchor(
    const DatabaseLocalPrivateSecurityLocatorV1& locator,
    const DatabaseLocalPrivateSecurityAnchorV1& anchor) {
  return locator.slot_ordinal == anchor.locator_slot &&
         locator.locator_generation == anchor.locator_generation &&
         locator.record_sha256 == anchor.locator_sha256;
}

bool LocatorFullFieldsEqual(
    const DatabaseLocalPrivateSecurityLocatorV1& left,
    const DatabaseLocalPrivateSecurityLocatorV1& right) {
  return left.slot_ordinal == right.slot_ordinal &&
         left.lineage == right.lineage &&
         left.database_uuid == right.database_uuid &&
         left.filespace_uuid == right.filespace_uuid &&
         left.relation_uuid == right.relation_uuid &&
         left.relation_generation == right.relation_generation &&
         left.locator_generation == right.locator_generation &&
         left.creator_transaction_uuid == right.creator_transaction_uuid &&
         left.creator_local_transaction_id ==
             right.creator_local_transaction_id &&
         left.head_page_uuid == right.head_page_uuid &&
         left.head_page_number == right.head_page_number &&
         left.head_page_generation == right.head_page_generation &&
         left.tail_page_uuid == right.tail_page_uuid &&
         left.tail_page_number == right.tail_page_number &&
         left.tail_page_generation == right.tail_page_generation &&
         left.chain_page_count == right.chain_page_count &&
         left.extent_min_page == right.extent_min_page &&
         left.extent_max_page == right.extent_max_page &&
         left.security_context_generation ==
             right.security_context_generation &&
         left.prior_locator_sha256 == right.prior_locator_sha256 &&
         left.record_sha256 == right.record_sha256;
}

bool LocatorCanonicalDigestValid(
    const DatabaseLocalPrivateSecurityLocatorV1& locator) {
  const auto encoded = EncodeDatabaseLocalPrivateSecurityLocatorV1(locator);
  return LoadDigest(encoded, 240) == locator.record_sha256;
}

bool ReadPageHeaderAndBody(disk::FileDevice* device,
                           u32 page_size,
                           u64 page_number,
                           disk::PageHeader* header,
                           std::vector<byte>* body,
                           DiagnosticRecord* diagnostic) {
  if (device == nullptr || header == nullptr || body == nullptr ||
      page_size <= disk::kPageHeaderSerializedBytes) {
    return false;
  }
  const auto offset = disk::CheckDevicePageOffset(page_size, page_number);
  if (!offset.ok()) {
    if (diagnostic != nullptr) *diagnostic = offset.diagnostic;
    return false;
  }
  disk::SerializedPageHeader serialized{};
  const auto header_read =
      device->ReadAt(offset.offset, serialized.data(), serialized.size());
  if (!header_read.ok()) {
    if (diagnostic != nullptr) *diagnostic = header_read.diagnostic;
    return false;
  }
  const auto parsed = disk::ParsePageHeader(serialized);
  if (!parsed.ok()) {
    if (diagnostic != nullptr) *diagnostic = parsed.diagnostic;
    return false;
  }
  const auto body_offset = disk::CheckDevicePageOffset(
      page_size, page_number, disk::kPageHeaderSerializedBytes);
  if (!body_offset.ok()) {
    if (diagnostic != nullptr) *diagnostic = body_offset.diagnostic;
    return false;
  }
  body->assign(page_size - disk::kPageHeaderSerializedBytes, 0);
  const auto body_read =
      device->ReadAt(body_offset.offset, body->data(), body->size());
  if (!body_read.ok()) {
    if (diagnostic != nullptr) *diagnostic = body_read.diagnostic;
    body->clear();
    return false;
  }
  *header = parsed.header;
  return true;
}

template <typename Bytes>
bool WriteLocatorPageRecord(disk::FileDevice* device,
                            u32 page_size,
                            u32 body_offset,
                            const Bytes& bytes,
                            DiagnosticRecord* diagnostic,
                            bool force_readback_mismatch = false) {
  if (device == nullptr || !device->is_open() || device->read_only()) {
    if (diagnostic != nullptr) {
      *diagnostic = MakeDatabaseLocalPrivateSecurityLocatorDiagnosticV1(
          LocatorErrorStatus(),
          kDatabaseLocalPrivateSecurityLocatorWriteFailed,
          "storage.database_local_private_security_locator.write_device_invalid");
    }
    return false;
  }
  const auto offset = disk::CheckDevicePageOffset(
      page_size, kDatabaseLocalPrivateSecurityRootPageNumberV1,
      disk::kPageHeaderSerializedBytes + body_offset);
  if (!offset.ok()) {
    if (diagnostic != nullptr) *diagnostic = offset.diagnostic;
    return false;
  }
  const auto write = device->WriteAt(offset.offset, bytes.data(), bytes.size());
  if (!write.ok()) {
    if (diagnostic != nullptr) *diagnostic = write.diagnostic;
    return false;
  }
  const auto sync = device->Sync();
  if (!sync.ok()) {
    if (diagnostic != nullptr) *diagnostic = sync.diagnostic;
    return false;
  }
  Bytes readback{};
  const auto read = device->ReadAt(offset.offset, readback.data(), readback.size());
  if (!read.ok()) {
    if (diagnostic != nullptr) *diagnostic = read.diagnostic;
    return false;
  }
  if (force_readback_mismatch && !readback.empty()) readback[0] ^= 0x01u;
  if (readback != bytes) {
    if (diagnostic != nullptr) {
      *diagnostic = MakeDatabaseLocalPrivateSecurityLocatorDiagnosticV1(
          LocatorErrorStatus(),
          kDatabaseLocalPrivateSecurityLocatorWriteFailed,
          "storage.database_local_private_security_locator.readback_mismatch");
    }
    return false;
  }
  return true;
}

bool AnchorFieldsMatchContext(
    const DatabaseLocalPrivateSecurityAnchorV1& anchor,
    const DatabaseLocalPrivateSecurityMarkerV1& marker) {
  return anchor.database_uuid == marker.database_uuid &&
         anchor.relation_uuid == marker.relation_uuid &&
         anchor.relation_generation == marker.relation_generation;
}

bool LocatorFieldsMatchContext(
    const DatabaseLocalPrivateSecurityLocatorV1& locator,
    const DatabaseLocalPrivateSecurityMarkerV1& marker,
    const disk::PageHeader& locator_page_header) {
  const bool lineage_matches_marker =
      (marker.flags == kMarkerFreshFlags &&
       locator.lineage ==
           DatabaseLocalPrivateSecurityLocatorLineageV1::fresh_bootstrap) ||
      (marker.flags == kMarkerMigratedFlags &&
       locator.lineage ==
           DatabaseLocalPrivateSecurityLocatorLineageV1::sealed_legacy_migration);
  const bool migrated_nonempty_baseline =
      locator.locator_generation == 1 && locator.chain_page_count != 0;
  return lineage_matches_marker &&
         (!migrated_nonempty_baseline ||
          (marker.flags == kMarkerMigratedFlags &&
           locator.lineage ==
               DatabaseLocalPrivateSecurityLocatorLineageV1::sealed_legacy_migration &&
           locator.creator_transaction_uuid.is_nil() &&
           locator.creator_local_transaction_id == 0 &&
           AllZero(locator.prior_locator_sha256))) &&
         locator.database_uuid == marker.database_uuid &&
         locator.filespace_uuid == locator_page_header.filespace_uuid &&
         locator.relation_uuid == marker.relation_uuid &&
         locator.relation_generation == marker.relation_generation;
}

constexpr std::size_t kRowBodyRelationUuidOffset = 40;

bool RefuseBatch(std::string* refusal, std::string reason) {
  if (refusal != nullptr) *refusal = std::move(reason);
  return false;
}

bool ParseExactU64(std::string_view text, u64* value) {
  if (value == nullptr || text.empty()) return false;
  u64 parsed = 0;
  const auto converted =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (converted.ec != std::errc{} ||
      converted.ptr != text.data() + text.size()) {
    return false;
  }
  *value = parsed;
  return true;
}

std::vector<std::string_view> SplitTabs(std::string_view line) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (begin <= line.size()) {
    const auto separator = line.find('\t', begin);
    if (separator == std::string_view::npos) {
      parts.push_back(line.substr(begin));
      break;
    }
    parts.push_back(line.substr(begin, separator - begin));
    begin = separator + 1;
  }
  return parts;
}

std::size_t ExactLifecycleFieldCount(std::string_view kind) {
  if (kind == "PRINCIPAL") return 10;
  if (kind == "ROLE") return 9;
  if (kind == "GROUP") return 9;
  if (kind == "MEMBERSHIP") return 10;
  if (kind == "GRANT") return 13;
  if (kind == "REVOKE") return 8;
  if (kind == "AUDIT") return 10;
  if (kind == "CACHE_INVALIDATE") return 6;
  if (kind == "ROW_POLICY") return 27;
  if (kind == kSuccessorKind) return 5;
  return 0;
}

std::size_t LifecycleGenerationField(std::string_view kind) {
  if (kind == "PRINCIPAL") return 8;
  if (kind == "ROLE" || kind == "GROUP" || kind == "REVOKE") return 7;
  if (kind == "MEMBERSHIP") return 8;
  if (kind == "GRANT") return 11;
  if (kind == "AUDIT") return 9;
  if (kind == "CACHE_INVALIDATE") return 5;
  if (kind == "ROW_POLICY") return 10;
  return std::numeric_limits<std::size_t>::max();
}

bool IsAuthorityLifecycleKind(std::string_view kind) {
  static constexpr std::array<std::string_view, 7> kKinds = {
      "PRINCIPAL", "ROLE", "GROUP", "MEMBERSHIP", "GRANT", "REVOKE",
      "ROW_POLICY"};
  return std::find(kKinds.begin(), kKinds.end(), kind) != kKinds.end();
}

bool ValidateLifecycleLine(std::string_view line,
                           std::string_view expected_kind,
                           u64 creator_local_transaction_id,
                           u64 expected_generation,
                           std::vector<std::string_view>* parts_out) {
  if (line.empty() || line.find('\n') != std::string_view::npos ||
      line.find('\r') != std::string_view::npos) {
    return false;
  }
  auto parts = SplitTabs(line);
  if (parts.size() != ExactLifecycleFieldCount(expected_kind) ||
      parts[0] != kLifecycleMagic || parts[1] != expected_kind) {
    return false;
  }
  u64 creator = 0;
  if (!ParseExactU64(parts[2], &creator) ||
      creator != creator_local_transaction_id) {
    return false;
  }
  const auto generation_field = LifecycleGenerationField(expected_kind);
  if (generation_field != std::numeric_limits<std::size_t>::max()) {
    u64 generation = 0;
    if (!ParseExactU64(parts[generation_field], &generation) ||
        generation == 0 || generation != expected_generation) {
      return false;
    }
  }
  if (parts_out != nullptr) *parts_out = std::move(parts);
  return true;
}

std::string NewlineTerminatedPayload(
    const std::vector<std::string>& events,
    std::size_t count = std::numeric_limits<std::size_t>::max()) {
  std::string payload;
  const auto limit = std::min(events.size(), count);
  for (std::size_t index = 0; index < limit; ++index) {
    payload.append(events[index]);
    payload.push_back('\n');
  }
  return payload;
}

bool DecodeLifecyclePayload(const std::vector<byte>& payload,
                            u32 event_count,
                            std::vector<std::string>* events) {
  if (events == nullptr || payload.empty() || payload.back() != '\n') {
    return false;
  }
  events->clear();
  std::size_t begin = 0;
  while (begin < payload.size()) {
    const auto found = std::find(
        payload.begin() + static_cast<std::ptrdiff_t>(begin), payload.end(),
        static_cast<byte>('\n'));
    if (found == payload.end()) return false;
    const auto end =
        static_cast<std::size_t>(std::distance(payload.begin(), found));
    if (end == begin) return false;
    events->emplace_back(
        reinterpret_cast<const char*>(payload.data() + begin), end - begin);
    begin = end + 1;
  }
  return events->size() == event_count;
}

bool ValidateLifecycleBatchInternal(
    const DatabaseLocalSecurityBatchEnvelopeV1& batch,
    std::string* refusal) {
  if (batch.creator_local_transaction_id == 0 ||
      batch.prior_security_context_generation == 0 ||
      batch.prior_security_context_generation ==
          std::numeric_limits<u64>::max() ||
      batch.successor_security_context_generation !=
          batch.prior_security_context_generation + 1 ||
      batch.events.size() != 4) {
    return RefuseBatch(refusal, "batch_identity_or_generation_invalid");
  }
  const auto actor_text = core_uuid::UuidToString(batch.actor_principal_uuid);
  const auto actor = core_uuid::ParseDurableEngineIdentityUuid(
      UuidKind::principal, actor_text);
  if (!actor.ok()) {
    return RefuseBatch(refusal, "batch_actor_principal_invalid");
  }

  const auto authority_parts = SplitTabs(batch.events[0]);
  if (authority_parts.size() < 2 ||
      !IsAuthorityLifecycleKind(authority_parts[1]) ||
      !ValidateLifecycleLine(
          batch.events[0], authority_parts[1],
          batch.creator_local_transaction_id,
          batch.successor_security_context_generation, nullptr)) {
    return RefuseBatch(refusal, "authority_event_invalid");
  }
  if (authority_parts[1] == "GRANT" && authority_parts[10] != "allow" &&
      authority_parts[10] != "deny") {
    return RefuseBatch(refusal, "grant_effect_invalid");
  }
  std::vector<std::string_view> audit_parts;
  if (!ValidateLifecycleLine(
          batch.events[1], "AUDIT", batch.creator_local_transaction_id,
          batch.successor_security_context_generation, &audit_parts) ||
      audit_parts[5] != actor_text || audit_parts[7] != "success") {
    return RefuseBatch(refusal, "audit_event_invalid");
  }
  if (!ValidateLifecycleLine(
          batch.events[2], "CACHE_INVALIDATE",
          batch.creator_local_transaction_id,
          batch.successor_security_context_generation, nullptr)) {
    return RefuseBatch(refusal, "cache_invalidation_event_invalid");
  }
  const auto successor_parts = SplitTabs(batch.events[3]);
  u64 successor_tx = 0;
  u64 successor_generation = 0;
  const std::string unsealed = NewlineTerminatedPayload(batch.events, 3);
  const auto digest = core_hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(unsealed.data()), unsealed.size());
  if (!digest.ok() ||
      successor_parts.size() != ExactLifecycleFieldCount(kSuccessorKind) ||
      successor_parts[0] != kLifecycleMagic ||
      successor_parts[1] != kSuccessorKind ||
      !ParseExactU64(successor_parts[2], &successor_tx) ||
      !ParseExactU64(successor_parts[3], &successor_generation) ||
      successor_tx != batch.creator_local_transaction_id ||
      successor_generation != batch.successor_security_context_generation ||
      successor_parts[4] !=
          std::string("security-context-successor:v1:sha256:") +
              core_hash::HexLower(digest.digest)) {
    return RefuseBatch(refusal,
                       "security_context_successor_evidence_invalid");
  }
  const bool predecessor_nil = batch.predecessor_page_uuid.is_nil();
  if (predecessor_nil != (batch.predecessor_page_number == 0) ||
      predecessor_nil != (batch.predecessor_page_generation == 0) ||
      (!predecessor_nil &&
       batch.predecessor_page_number <
           kDatabaseLocalPrivateSecurityFirstDataPageNumberV1)) {
    return RefuseBatch(refusal, "batch_predecessor_identity_invalid");
  }
  return true;
}

struct LegacySecurityBatch {
  Uuid database_uuid;
  Uuid relation_uuid;
  Uuid transaction_uuid;
  Uuid actor_principal_uuid;
  u64 creator_local_transaction_id = 0;
  u64 prior_security_context_generation = 0;
  u64 successor_security_context_generation = 0;
  u32 event_count = 0;
  std::vector<byte> payload;
  std::vector<std::string> events;
};

struct LegacySecurityPage {
  disk::PageHeader header;
  page::RowDataPageBody body;
  LegacySecurityBatch batch;
  bool committed_visible = false;
};

bool DecodeLegacySecurityBatch(const std::vector<byte>& bytes,
                               LegacySecurityBatch* batch) {
  if (batch == nullptr || bytes.size() < kLegacySecurityBatchFixedBytes ||
      !std::equal(kSecurityBatchMagic.begin(), kSecurityBatchMagic.end(),
                  bytes.begin()) ||
      scratchbird::core::platform::LoadLittle16(bytes.data() + 8) !=
          kSecurityBatchMajor ||
      scratchbird::core::platform::LoadLittle16(bytes.data() + 10) !=
          kLegacySecurityBatchMinor) {
    return false;
  }
  batch->database_uuid = LoadUuid(bytes, 12);
  batch->relation_uuid = LoadUuid(bytes, 28);
  batch->transaction_uuid = LoadUuid(bytes, 44);
  batch->actor_principal_uuid = LoadUuid(bytes, 60);
  batch->creator_local_transaction_id = LoadU64(bytes, 76);
  batch->prior_security_context_generation = LoadU64(bytes, 84);
  batch->successor_security_context_generation = LoadU64(bytes, 92);
  batch->event_count = LoadU32(bytes, 100);
  const u32 payload_bytes = LoadU32(bytes, 104);
  if (batch->event_count != 4 || payload_bytes == 0 ||
      payload_bytes > bytes.size() - kLegacySecurityBatchFixedBytes ||
      kLegacySecurityBatchFixedBytes + payload_bytes != bytes.size()) {
    return false;
  }
  const auto stored_digest = LoadDigest(bytes, 108);
  batch->payload.assign(bytes.begin() + kLegacySecurityBatchFixedBytes,
                        bytes.end());
  const auto digest = core_hash::ComputeSha256Digest(batch->payload);
  if (!digest.ok() || digest.digest != stored_digest ||
      !DecodeLifecyclePayload(batch->payload, batch->event_count,
                              &batch->events)) {
    return false;
  }
  DatabaseLocalSecurityBatchEnvelopeV1 exact;
  exact.database_uuid = batch->database_uuid;
  exact.relation_uuid = batch->relation_uuid;
  exact.transaction_uuid = batch->transaction_uuid;
  exact.actor_principal_uuid = batch->actor_principal_uuid;
  exact.creator_local_transaction_id =
      batch->creator_local_transaction_id;
  exact.prior_security_context_generation =
      batch->prior_security_context_generation;
  exact.successor_security_context_generation =
      batch->successor_security_context_generation;
  exact.events = batch->events;
  return ValidateLifecycleBatchInternal(exact, nullptr);
}

std::vector<byte> EncodeCurrentSecurityBatch(
    const LegacySecurityBatch& batch,
    const disk::PageHeader* predecessor) {
  DatabaseLocalSecurityBatchEnvelopeV1 current;
  current.database_uuid = batch.database_uuid;
  current.relation_uuid = batch.relation_uuid;
  current.transaction_uuid = batch.transaction_uuid;
  current.actor_principal_uuid = batch.actor_principal_uuid;
  current.creator_local_transaction_id = batch.creator_local_transaction_id;
  current.prior_security_context_generation =
      batch.prior_security_context_generation;
  current.successor_security_context_generation =
      batch.successor_security_context_generation;
  current.events = batch.events;
  if (predecessor != nullptr) {
    current.predecessor_page_uuid = predecessor->page_uuid;
    current.predecessor_page_number = predecessor->page_number;
    current.predecessor_page_generation = predecessor->page_generation;
  }
  return EncodeDatabaseLocalSecurityBatchEnvelopeV1(current);
}

bool RawBodyClaimsSecurity(const std::vector<byte>& body,
                           const Uuid& database_uuid,
                           const Uuid& relation_uuid) {
  if (body.size() >= kRowBodyRelationUuidOffset + relation_uuid.bytes.size() &&
      std::equal(relation_uuid.bytes.begin(), relation_uuid.bytes.end(),
                 body.begin() + kRowBodyRelationUuidOffset)) {
    return true;
  }
  for (std::size_t offset = 0;
       offset + kLegacySecurityBatchFixedBytes <= body.size(); ++offset) {
    if (std::equal(kSecurityBatchMagic.begin(), kSecurityBatchMagic.end(),
                   body.begin() + static_cast<std::ptrdiff_t>(offset)) &&
        std::equal(database_uuid.bytes.begin(), database_uuid.bytes.end(),
                   body.begin() + static_cast<std::ptrdiff_t>(offset + 12)) &&
        std::equal(relation_uuid.bytes.begin(), relation_uuid.bytes.end(),
                   body.begin() + static_cast<std::ptrdiff_t>(offset + 28))) {
      return true;
    }
  }
  return false;
}

bool ReadRawPage(disk::FileDevice* device,
                 u32 page_size,
                 u64 page_number,
                 disk::SerializedPageHeader* header,
                 std::vector<byte>* body,
                 DiagnosticRecord* diagnostic) {
  if (device == nullptr || header == nullptr || body == nullptr ||
      page_size <= disk::kPageHeaderSerializedBytes) {
    return false;
  }
  const auto offset = disk::CheckDevicePageOffset(page_size, page_number);
  const auto body_offset = disk::CheckDevicePageOffset(
      page_size, page_number, disk::kPageHeaderSerializedBytes);
  if (!offset.ok() || !body_offset.ok()) {
    if (diagnostic != nullptr) {
      *diagnostic = !offset.ok() ? offset.diagnostic : body_offset.diagnostic;
    }
    return false;
  }
  const auto header_read =
      device->ReadAt(offset.offset, header->data(), header->size());
  if (!header_read.ok()) {
    if (diagnostic != nullptr) *diagnostic = header_read.diagnostic;
    return false;
  }
  body->assign(page_size - disk::kPageHeaderSerializedBytes, 0);
  const auto body_read =
      device->ReadAt(body_offset.offset, body->data(), body->size());
  if (!body_read.ok()) {
    if (diagnostic != nullptr) *diagnostic = body_read.diagnostic;
    body->clear();
    return false;
  }
  return true;
}

bool WriteMigratedSecurityPage(disk::FileDevice* device,
                               u32 page_size,
                               LegacySecurityPage* security_page,
                               const disk::PageHeader* predecessor,
                               DiagnosticRecord* diagnostic) {
  if (device == nullptr || security_page == nullptr ||
      security_page->body.rows.size() != 1 ||
      security_page->body.rows[0].cells.size() != 1) {
    return false;
  }
  auto encoded = EncodeCurrentSecurityBatch(security_page->batch, predecessor);
  if (encoded.empty()) return false;
  security_page->body.rows[0].cells[0].value.payload = std::move(encoded);
  security_page->body.next_page_number =
      predecessor == nullptr ? 0 : predecessor->page_number;
  const auto built = page::BuildRowDataPageBody(security_page->body, page_size);
  if (!built.ok()) {
    if (diagnostic != nullptr) *diagnostic = built.diagnostic;
    return false;
  }
  const auto offset = disk::CheckDevicePageOffset(
      page_size, security_page->header.page_number,
      disk::kPageHeaderSerializedBytes);
  if (!offset.ok()) {
    if (diagnostic != nullptr) *diagnostic = offset.diagnostic;
    return false;
  }
  const auto write =
      device->WriteAt(offset.offset, built.serialized.data(),
                      built.serialized.size());
  if (!write.ok()) {
    if (diagnostic != nullptr) *diagnostic = write.diagnostic;
    return false;
  }
  std::vector<byte> readback(built.serialized.size(), 0);
  const auto read =
      device->ReadAt(offset.offset, readback.data(), readback.size());
  if (!read.ok()) {
    if (diagnostic != nullptr) *diagnostic = read.diagnostic;
    return false;
  }
  const auto parsed = page::ParseRowDataPageBody(
      readback, security_page->header.page_number);
  return parsed.ok() &&
         parsed.body.next_page_number == security_page->body.next_page_number;
}

bool ValidateMigratedSecurityChainReadback(
    disk::FileDevice* device,
    u32 page_size,
    const Uuid& database_uuid,
    const Uuid& filespace_uuid,
    const Uuid& relation_uuid,
    u64 bootstrap_security_context_generation,
    const DatabaseLocalPrivateSecurityLocatorV1& locator,
    const mga::LocalTransactionInventory& inventory,
    std::string* refusal) {
  if (locator.chain_page_count == 0) {
    if (!locator.head_page_uuid.is_nil() || locator.head_page_number != 0 ||
        locator.head_page_generation != 0 || !locator.tail_page_uuid.is_nil() ||
        locator.tail_page_number != 0 || locator.tail_page_generation != 0 ||
        locator.extent_min_page != 0 || locator.extent_max_page != 0 ||
        locator.security_context_generation !=
            bootstrap_security_context_generation) {
      return RefuseBatch(refusal, "empty_migrated_chain_readback_invalid");
    }
    return true;
  }
  if (locator.chain_page_count >
      static_cast<u64>(std::numeric_limits<std::size_t>::max())) {
    return RefuseBatch(refusal, "migrated_chain_count_extent_invalid");
  }
  std::set<u64> visited;
  Uuid expected_page_uuid = locator.head_page_uuid;
  u64 expected_page_number = locator.head_page_number;
  u64 expected_page_generation = locator.head_page_generation;
  u64 expected_security_generation = locator.security_context_generation;
  u64 observed_count = 0;
  u64 observed_min = std::numeric_limits<u64>::max();
  u64 observed_max = 0;
  disk::PageHeader last_header;

  while (expected_page_number != 0) {
    if (observed_count >= locator.chain_page_count ||
        !visited.insert(expected_page_number).second) {
      return RefuseBatch(refusal, "migrated_chain_count_or_cycle_invalid");
    }
    disk::PageHeader header;
    std::vector<byte> body_bytes;
    DiagnosticRecord diagnostic;
    if (!ReadPageHeaderAndBody(device, page_size, expected_page_number,
                               &header, &body_bytes, &diagnostic) ||
        header.page_type != disk::PageType::row_data ||
        header.database_uuid != database_uuid ||
        header.filespace_uuid != filespace_uuid ||
        header.page_number != expected_page_number ||
        header.page_uuid != expected_page_uuid ||
        header.page_generation != expected_page_generation) {
      return RefuseBatch(refusal, "migrated_chain_outer_identity_invalid");
    }
    const auto parsed = page::ParseRowDataPageBody(body_bytes,
                                                   expected_page_number);
    if (!parsed.ok() || parsed.body.relation_uuid.kind != UuidKind::object ||
        parsed.body.relation_uuid.value != relation_uuid ||
        parsed.body.segment_id != 1 || parsed.body.segment_generation != 1 ||
        parsed.body.page_number != expected_page_number ||
        parsed.body.page_generation != expected_page_generation ||
        parsed.body.compaction_generation != 1 ||
        parsed.body.rows.size() != 1 || parsed.body.slots.size() != 1) {
      return RefuseBatch(refusal, "migrated_chain_row_body_shape_invalid");
    }
    const auto& row = parsed.body.rows[0];
    if (row.deleted || row.row_uuid.kind != UuidKind::row ||
        !row.row_uuid.valid() ||
        row.transaction_uuid.kind != UuidKind::transaction ||
        !row.transaction_uuid.valid() || row.local_transaction_id == 0 ||
        row.internal_row_ordinal != 1 || row.stable_slot_id != 1 ||
        row.row_version != 1 || row.previous_row_version != 0 ||
        row.next_row_version != 0 || row.cells.size() != 1 ||
        parsed.body.slots[0].stable_slot_id != 1 ||
        parsed.body.slots[0].deleted || row.cells[0].column_ordinal != 1 ||
        row.cells[0].value.type_id !=
            scratchbird::core::datatypes::CanonicalTypeId::binary ||
        row.cells[0].value.is_null ||
        row.cells[0].value.payload_is_toast_reference) {
      return RefuseBatch(refusal, "migrated_chain_row_shape_invalid");
    }
    DatabaseLocalSecurityBatchEnvelopeV1 batch;
    if (!DecodeDatabaseLocalSecurityBatchEnvelopeV1(
            row.cells[0].value.payload, &batch, refusal) ||
        batch.database_uuid != database_uuid ||
        batch.relation_uuid != relation_uuid ||
        batch.transaction_uuid != row.transaction_uuid.value ||
        batch.creator_local_transaction_id != row.local_transaction_id ||
        batch.successor_security_context_generation !=
            expected_security_generation ||
        parsed.body.next_page_number != batch.predecessor_page_number) {
      return RefuseBatch(refusal, "migrated_chain_batch_cross_bind_invalid");
    }
    const auto creator = mga::LookupLocalTransaction(
        inventory,
        mga::MakeLocalTransactionId(batch.creator_local_transaction_id));
    if (!creator.ok() ||
        creator.entry.identity.transaction_uuid.kind != UuidKind::transaction ||
        creator.entry.identity.transaction_uuid.value !=
            batch.transaction_uuid ||
        (creator.entry.state != mga::TransactionState::committed &&
         creator.entry.state != mga::TransactionState::archived)) {
      return RefuseBatch(refusal, "migrated_chain_mga_visibility_invalid");
    }
    ++observed_count;
    observed_min = std::min(observed_min, expected_page_number);
    observed_max = std::max(observed_max, expected_page_number);
    last_header = header;
    if (batch.predecessor_page_number == 0) {
      if (!batch.predecessor_page_uuid.is_nil() ||
          batch.predecessor_page_generation != 0 ||
          batch.prior_security_context_generation !=
              bootstrap_security_context_generation) {
        return RefuseBatch(refusal, "migrated_chain_tail_invalid");
      }
      expected_page_number = 0;
    } else {
      expected_page_uuid = batch.predecessor_page_uuid;
      expected_page_number = batch.predecessor_page_number;
      expected_page_generation = batch.predecessor_page_generation;
      expected_security_generation =
          batch.prior_security_context_generation;
    }
  }
  return observed_count == locator.chain_page_count &&
         observed_min == locator.extent_min_page &&
         observed_max == locator.extent_max_page &&
         last_header.page_uuid == locator.tail_page_uuid &&
         last_header.page_number == locator.tail_page_number &&
         last_header.page_generation == locator.tail_page_generation
             ? true
             : RefuseBatch(refusal,
                           "migrated_chain_locator_extent_or_tail_invalid");
}

}  // namespace

bool ValidateDatabaseLocalSecurityLifecycleBatchV1(
    const DatabaseLocalSecurityBatchEnvelopeV1& batch,
    std::string* refusal) {
  return ValidateLifecycleBatchInternal(batch, refusal);
}

std::vector<byte> EncodeDatabaseLocalSecurityBatchEnvelopeV1(
    const DatabaseLocalSecurityBatchEnvelopeV1& batch,
    std::string* refusal) {
  if (!ValidateLifecycleBatchInternal(batch, refusal)) return {};
  const std::string payload = NewlineTerminatedPayload(batch.events);
  if (payload.empty() ||
      payload.size() > std::numeric_limits<u32>::max()) {
    RefuseBatch(refusal, "batch_payload_size_invalid");
    return {};
  }
  const auto digest = core_hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(payload.data()), payload.size());
  if (!digest.ok()) {
    RefuseBatch(refusal, "batch_payload_digest_failed");
    return {};
  }
  std::vector<byte> encoded(kCurrentSecurityBatchFixedBytes + payload.size(),
                            0);
  std::copy(kSecurityBatchMagic.begin(), kSecurityBatchMagic.end(),
            encoded.begin());
  StoreU16(&encoded, 8, kSecurityBatchMajor);
  StoreU16(&encoded, 10, kCurrentSecurityBatchMinor);
  StoreUuid(&encoded, 12, batch.database_uuid);
  StoreUuid(&encoded, 28, batch.relation_uuid);
  StoreUuid(&encoded, 44, batch.transaction_uuid);
  StoreUuid(&encoded, 60, batch.actor_principal_uuid);
  StoreU64(&encoded, 76, batch.creator_local_transaction_id);
  StoreU64(&encoded, 84, batch.prior_security_context_generation);
  StoreU64(&encoded, 92, batch.successor_security_context_generation);
  StoreU32(&encoded, 100, static_cast<u32>(batch.events.size()));
  StoreU32(&encoded, 104, static_cast<u32>(payload.size()));
  StoreUuid(&encoded, 108, batch.predecessor_page_uuid);
  StoreU64(&encoded, 124, batch.predecessor_page_number);
  StoreU64(&encoded, 132, batch.predecessor_page_generation);
  StoreDigest(&encoded, 140, digest.digest);
  std::copy(payload.begin(), payload.end(),
            encoded.begin() + kCurrentSecurityBatchFixedBytes);
  return encoded;
}

bool DecodeDatabaseLocalSecurityBatchEnvelopeV1(
    const std::vector<byte>& encoded,
    DatabaseLocalSecurityBatchEnvelopeV1* batch,
    std::string* refusal) {
  if (batch == nullptr || encoded.size() < kCurrentSecurityBatchFixedBytes ||
      !std::equal(kSecurityBatchMagic.begin(), kSecurityBatchMagic.end(),
                  encoded.begin()) ||
      LoadU16(encoded, 8) != kSecurityBatchMajor ||
      LoadU16(encoded, 10) != kCurrentSecurityBatchMinor) {
    return RefuseBatch(refusal, "batch_magic_version_or_length_invalid");
  }
  DatabaseLocalSecurityBatchEnvelopeV1 decoded;
  decoded.database_uuid = LoadUuid(encoded, 12);
  decoded.relation_uuid = LoadUuid(encoded, 28);
  decoded.transaction_uuid = LoadUuid(encoded, 44);
  decoded.actor_principal_uuid = LoadUuid(encoded, 60);
  decoded.creator_local_transaction_id = LoadU64(encoded, 76);
  decoded.prior_security_context_generation = LoadU64(encoded, 84);
  decoded.successor_security_context_generation = LoadU64(encoded, 92);
  const u32 event_count = LoadU32(encoded, 100);
  const u32 payload_size = LoadU32(encoded, 104);
  decoded.predecessor_page_uuid = LoadUuid(encoded, 108);
  decoded.predecessor_page_number = LoadU64(encoded, 124);
  decoded.predecessor_page_generation = LoadU64(encoded, 132);
  if (event_count != 4 || payload_size == 0 ||
      payload_size != encoded.size() - kCurrentSecurityBatchFixedBytes) {
    return RefuseBatch(refusal, "batch_payload_shape_invalid");
  }
  const auto stored_digest = LoadDigest(encoded, 140);
  const std::vector<byte> payload(
      encoded.begin() + kCurrentSecurityBatchFixedBytes, encoded.end());
  const auto digest = core_hash::ComputeSha256Digest(payload);
  if (!digest.ok() || digest.digest != stored_digest ||
      !DecodeLifecyclePayload(payload, event_count, &decoded.events)) {
    return RefuseBatch(refusal, "batch_payload_digest_or_lines_invalid");
  }
  if (!ValidateLifecycleBatchInternal(decoded, refusal)) return false;
  *batch = std::move(decoded);
  return true;
}

SerializedDatabaseLocalPrivateSecurityMarkerV1
EncodeDatabaseLocalPrivateSecurityMarkerV1(
    const DatabaseLocalPrivateSecurityMarkerV1& marker) {
  SerializedDatabaseLocalPrivateSecurityMarkerV1 bytes{};
  std::copy(kMarkerMagic.begin(), kMarkerMagic.end(), bytes.begin());
  StoreU16(&bytes, 8, kFormatMajor);
  StoreU16(&bytes, 10, kFormatMinor);
  StoreU32(&bytes, 12, kDatabaseLocalPrivateSecurityMarkerBytesV1);
  StoreU32(&bytes, 16, marker.flags);
  StoreU32(&bytes, 20, 0);
  StoreUuid(&bytes, 24, marker.database_uuid);
  StoreUuid(&bytes, 40, marker.relation_uuid);
  StoreU64(&bytes, 56, marker.relation_generation);
  StoreU64(&bytes, 64, marker.migration_scan_count);
  StoreUuid(&bytes, 72, marker.locator_page_uuid);
  StoreU64(&bytes, 88, marker.locator_page_number);
  StoreU64(&bytes, 96, marker.locator_page_generation);
  StoreU64(&bytes, 104, marker.initial_locator_generation);
  StoreDigest(&bytes, 112, marker.initial_locator_sha256);
  StoreDigest(&bytes, 144, HashRecord(kMarkerHashDomain, bytes, 144));
  return bytes;
}

SerializedDatabaseLocalPrivateSecurityAnchorV1
EncodeDatabaseLocalPrivateSecurityAnchorV1(
    const DatabaseLocalPrivateSecurityAnchorV1& anchor) {
  SerializedDatabaseLocalPrivateSecurityAnchorV1 bytes{};
  std::copy(kAnchorMagic.begin(), kAnchorMagic.end(), bytes.begin());
  StoreU16(&bytes, 8, kFormatMajor);
  StoreU16(&bytes, 10, kFormatMinor);
  StoreU32(&bytes, 12, kDatabaseLocalPrivateSecurityAnchorBytesV1);
  StoreU32(&bytes, 16, anchor.copy_ordinal);
  StoreU32(&bytes, 20, anchor.flags);
  StoreUuid(&bytes, 24, anchor.database_uuid);
  StoreUuid(&bytes, 40, anchor.relation_uuid);
  StoreU64(&bytes, 56, anchor.relation_generation);
  StoreU64(&bytes, 64, anchor.anchor_generation);
  StoreU32(&bytes, 72, anchor.locator_slot);
  StoreU32(&bytes, 76, 0);
  StoreU64(&bytes, 80, anchor.locator_generation);
  StoreDigest(&bytes, 88, anchor.locator_sha256);
  StoreDigest(&bytes, 120, HashRecord(kAnchorHashDomain, bytes, 120));
  return bytes;
}

SerializedDatabaseLocalPrivateSecurityLocatorV1
EncodeDatabaseLocalPrivateSecurityLocatorV1(
    const DatabaseLocalPrivateSecurityLocatorV1& locator) {
  SerializedDatabaseLocalPrivateSecurityLocatorV1 bytes{};
  std::copy(kLocatorMagic.begin(), kLocatorMagic.end(), bytes.begin());
  StoreU16(&bytes, 8, kFormatMajor);
  StoreU16(&bytes, 10, kFormatMinor);
  StoreU32(&bytes, 12, kDatabaseLocalPrivateSecurityLocatorBytesV1);
  StoreU32(&bytes, 16, locator.slot_ordinal);
  StoreU32(&bytes, 20, static_cast<u32>(locator.lineage));
  StoreUuid(&bytes, 24, locator.database_uuid);
  StoreUuid(&bytes, 40, locator.filespace_uuid);
  StoreUuid(&bytes, 56, locator.relation_uuid);
  StoreU64(&bytes, 72, locator.relation_generation);
  StoreU64(&bytes, 80, locator.locator_generation);
  StoreUuid(&bytes, 88, locator.creator_transaction_uuid);
  StoreU64(&bytes, 104, locator.creator_local_transaction_id);
  StoreUuid(&bytes, 112, locator.head_page_uuid);
  StoreU64(&bytes, 128, locator.head_page_number);
  StoreU64(&bytes, 136, locator.head_page_generation);
  StoreUuid(&bytes, 144, locator.tail_page_uuid);
  StoreU64(&bytes, 160, locator.tail_page_number);
  StoreU64(&bytes, 168, locator.tail_page_generation);
  StoreU64(&bytes, 176, locator.chain_page_count);
  StoreU64(&bytes, 184, locator.extent_min_page);
  StoreU64(&bytes, 192, locator.extent_max_page);
  StoreU64(&bytes, 200, locator.security_context_generation);
  StoreDigest(&bytes, 208, locator.prior_locator_sha256);
  StoreDigest(&bytes, 240, HashRecord(kLocatorHashDomain, bytes, 240));
  return bytes;
}

DatabaseLocalPrivateSecurityLocatorInspectResultV1
InspectDatabaseLocalPrivateSecurityLocatorV1(
    const DatabaseLocalPrivateSecurityLocatorInspectRequestV1& request) {
  if (request.device == nullptr || !request.device->is_open() ||
      !SameUuid(request.expected_database_uuid,
                request.expected_database_uuid.value, UuidKind::database) ||
      !disk::IsSupportedDatabasePageSize(request.page_size) ||
      request.page_size <= disk::kPageHeaderSerializedBytes + 880) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.request_invalid",
        "inspect_request_invalid");
  }

  disk::PageHeader page_header;
  std::vector<byte> body;
  DiagnosticRecord io_diagnostic;
  if (!ReadPageHeaderAndBody(
          request.device, request.page_size,
          kDatabaseLocalPrivateSecurityRootPageNumberV1, &page_header, &body,
          &io_diagnostic)) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        io_diagnostic.status, std::move(io_diagnostic));
  }

  DatabaseLocalPrivateSecurityLocatorInspectResultV1 result;
  result.metrics.locator_page_reads = 1;
  result.locator_page_header = page_header;
  const bool marker_zero = AllZero(request.marker_bytes);
  if (marker_zero) {
    if (request.admit_exact_legacy_absence &&
        page_header.page_type == disk::PageType::bootstrap_reserved &&
        page_header.page_number ==
            kDatabaseLocalPrivateSecurityRootPageNumberV1 &&
        page_header.page_size == request.page_size &&
        page_header.database_uuid == request.expected_database_uuid.value &&
        AllZero(body)) {
      result.status = LocatorOkStatus();
      result.classification =
          DatabaseLocalPrivateSecurityLocatorClassV1::exact_legacy_absence;
      return result;
    }
    return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        kDatabaseLocalPrivateSecurityLocatorMissing,
        "storage.database_local_private_security_locator.marker_missing",
        "sealed_marker_absent");
  }

  std::string refusal;
  if (!DecodeMarker(request.marker_bytes, &result.marker, &refusal)) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.marker_invalid",
        std::move(refusal));
  }
  if (result.marker.database_uuid != request.expected_database_uuid.value ||
      page_header.page_type != disk::PageType::security_root ||
      page_header.page_size != request.page_size ||
      page_header.page_number !=
          kDatabaseLocalPrivateSecurityRootPageNumberV1 ||
      page_header.database_uuid != request.expected_database_uuid.value ||
      page_header.page_uuid != result.marker.locator_page_uuid ||
      page_header.page_generation != result.marker.locator_page_generation ||
      !BodyHasOnlyAssignedBytes(body)) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.identity_invalid",
        "marker_page_or_reserved_bytes_mismatch");
  }

  std::array<DecodedAnchor, 2> anchors{};
  std::array<DecodedLocator, 2> locators{};
  for (u32 index = 0; index < 2; ++index) {
    anchors[index] = DecodeAnchor(
        Slice<kDatabaseLocalPrivateSecurityAnchorBytesV1>(
            body, kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[index]),
        index);
    locators[index] = DecodeLocator(
        Slice<kDatabaseLocalPrivateSecurityLocatorBytesV1>(
            body, kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[index]),
        index);
    result.anchors[index] = anchors[index].value;
    result.locators[index] = locators[index].value;
    result.anchor_valid[index] =
        anchors[index].shape_valid && anchors[index].digest_valid &&
        AnchorFieldsMatchContext(anchors[index].value, result.marker);
    result.locator_shape_valid[index] = locators[index].shape_valid;
    result.locator_digest_valid[index] = locators[index].digest_valid;
    result.locator_zero[index] = locators[index].zero;
    result.locator_context_valid[index] =
        locators[index].shape_valid && locators[index].digest_valid &&
        LocatorFieldsMatchContext(locators[index].value, result.marker,
                                  page_header);
  }

  u32 authoritative_anchor = 0;
  const u32 valid_anchor_count =
      static_cast<u32>(result.anchor_valid[0]) +
      static_cast<u32>(result.anchor_valid[1]);
  if (valid_anchor_count == 0) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.anchor_corrupt",
        "no_canonical_anchor_copy");
  }
  if (valid_anchor_count == 1) {
    authoritative_anchor = result.anchor_valid[0] ? 0 : 1;
  } else if (anchors[0].value.anchor_generation ==
             anchors[1].value.anchor_generation) {
    if (!SameAnchorLogicalFields(anchors[0].value, anchors[1].value)) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.anchor_divergent",
          "equal_generation_anchor_divergence");
    }
    authoritative_anchor = 0;
  } else {
    const u64 lower = std::min(anchors[0].value.anchor_generation,
                               anchors[1].value.anchor_generation);
    const u64 higher = std::max(anchors[0].value.anchor_generation,
                                anchors[1].value.anchor_generation);
    if (lower == std::numeric_limits<u64>::max() || higher != lower + 1) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.anchor_gap",
          "anchor_generation_gap_or_digest_invalid");
    }
    const u32 newer = anchors[0].value.anchor_generation >
                              anchors[1].value.anchor_generation
                          ? 0
                          : 1;
    const u32 older = newer == 0 ? 1 : 0;
    const auto& older_anchor = anchors[older].value;
    const auto& older_locator = locators[older_anchor.locator_slot];
    if (!result.locator_context_valid[older_anchor.locator_slot] ||
        !LocatorMatchesAnchor(older_locator.value, older_anchor)) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.torn_anchor_invalid",
          "older_authoritative_anchor_locator_invalid");
    }
    // A single successor-anchor write is not publication. The independently
    // authenticated older anchor/locator remains authoritative even when the
    // newer target is absent, torn, corrupt, or not yet MGA-visible.
    authoritative_anchor = older;
  }

  const auto& anchor = anchors[authoritative_anchor].value;
  const auto& locator = locators[anchor.locator_slot];
  if (!result.anchor_valid[authoritative_anchor] ||
      !result.locator_context_valid[anchor.locator_slot] ||
      !LocatorMatchesAnchor(locator.value, anchor)) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.current_invalid",
        "anchored_locator_invalid");
  }
  if (anchor.anchor_generation == 1 &&
      (anchor.locator_generation != result.marker.initial_locator_generation ||
       anchor.locator_sha256 != result.marker.initial_locator_sha256)) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInspectResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.initial_invalid",
        "marker_initial_locator_mismatch");
  }

  result.status = LocatorOkStatus();
  result.classification =
      DatabaseLocalPrivateSecurityLocatorClassV1::sealed_current;
  result.anchored_locator_slot = anchor.locator_slot;
  result.anchored_locator_generation = anchor.locator_generation;
  result.metrics.locator_migration_count = result.marker.migration_scan_count;
  return result;
}

DatabaseLocalPrivateSecurityLocatorVisibilityResultV1
SelectDatabaseLocalPrivateSecurityLocatorForVisibilityV1(
    const DatabaseLocalPrivateSecurityLocatorVisibilityRequestV1& request) {
  if (request.inspected == nullptr || request.inventory == nullptr ||
      !request.inspected->ok() ||
      request.inspected->classification !=
          DatabaseLocalPrivateSecurityLocatorClassV1::sealed_current ||
      request.inspected->anchored_locator_slot >= 2 ||
      request.transaction_inventory_filespace_uuid.kind !=
          UuidKind::filespace ||
      !request.transaction_inventory_filespace_uuid.valid() ||
      request.transaction_inventory_filespace_uuid.value !=
          request.inspected->locator_page_header.filespace_uuid) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorVisibilityResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.visibility_invalid",
        "visibility_request_invalid");
  }
  const auto& inspected = *request.inspected;
  const auto& anchored =
      inspected.locators[inspected.anchored_locator_slot];
  DatabaseLocalPrivateSecurityLocatorVisibilityResultV1 result;
  result.selected_locator = anchored;
  result.selected_locator_slot = inspected.anchored_locator_slot;
  if (anchored.locator_generation == 1) {
    if (!anchored.creator_transaction_uuid.is_nil() ||
        anchored.creator_local_transaction_id != 0) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorVisibilityResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.bootstrap_invalid",
          "generation_one_creator_not_nil");
    }
    result.status = LocatorOkStatus();
    result.anchored_locator_visible = true;
    return result;
  }

  bool creator_visible = false;
  const auto found = mga::LookupLocalTransaction(
      *request.inventory,
      mga::MakeLocalTransactionId(anchored.creator_local_transaction_id));
  if (found.ok() &&
      found.entry.identity.transaction_uuid.kind == UuidKind::transaction &&
      found.entry.identity.transaction_uuid.value ==
          anchored.creator_transaction_uuid) {
    const bool committed =
        found.entry.state == mga::TransactionState::committed ||
        found.entry.state == mga::TransactionState::archived;
    if (committed) {
      creator_visible = request.use_latest_committed_snapshot ||
                        ((!request.visibility_snapshot
                               .visible_through_local_transaction_id_is_boundary &&
                          request.visibility_snapshot
                                  .visible_through_local_transaction_id ==
                              mga::kInvalidLocalTransactionId) ||
                         anchored.creator_local_transaction_id <=
                             request.visibility_snapshot
                                 .visible_through_local_transaction_id);
    } else if (!request.use_latest_committed_snapshot &&
               found.entry.state == mga::TransactionState::active &&
               request.visibility_snapshot.allow_reader_own_uncommitted &&
               request.reader_local_transaction_id.value ==
                   anchored.creator_local_transaction_id &&
               request.reader_transaction_uuid.kind == UuidKind::transaction &&
               request.reader_transaction_uuid.value ==
                   anchored.creator_transaction_uuid) {
      creator_visible = true;
    }
  }

  if (creator_visible) {
    result.status = LocatorOkStatus();
    result.anchored_locator_visible = true;
    return result;
  }

  const u32 prior_slot = inspected.anchored_locator_slot == 0 ? 1 : 0;
  const auto& prior = inspected.locators[prior_slot];
  if (!inspected.locator_shape_valid[prior_slot] ||
      !inspected.locator_digest_valid[prior_slot] ||
      !inspected.locator_context_valid[prior_slot] ||
      !LocatorCanonicalDigestValid(prior) ||
      prior.locator_generation == 0 ||
      prior.locator_generation == std::numeric_limits<u64>::max() ||
      prior.locator_generation + 1 != anchored.locator_generation ||
      prior.record_sha256 != anchored.prior_locator_sha256 ||
      prior.database_uuid != anchored.database_uuid ||
      prior.filespace_uuid != anchored.filespace_uuid ||
      prior.relation_uuid != anchored.relation_uuid ||
      prior.relation_generation != anchored.relation_generation ||
      prior.lineage != anchored.lineage ||
      anchored.security_context_generation <=
          prior.security_context_generation ||
      anchored.chain_page_count <= prior.chain_page_count) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorVisibilityResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.prior_invalid",
        "digest_linked_prior_locator_missing");
  }
  result.status = LocatorOkStatus();
  result.selected_locator = prior;
  result.selected_locator_slot = prior_slot;
  result.selected_digest_linked_prior = true;
  return result;
}

DatabaseLocalPrivateSecurityLocatorInitializeResultV1
InitializeFreshDatabaseLocalPrivateSecurityLocatorV1(
    const DatabaseLocalPrivateSecurityLocatorInitializeRequestV1& request) {
  if (request.device == nullptr || !request.device->is_open() ||
      request.device->read_only() ||
      request.database_uuid.kind != UuidKind::database ||
      !request.database_uuid.valid() ||
      !disk::IsSupportedDatabasePageSize(request.page_size) ||
      request.page_size <= disk::kPageHeaderSerializedBytes + 880 ||
      request.bootstrap_security_context_generation == 0) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInitializeResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.initialize_invalid",
        "fresh_initialize_request_invalid");
  }
  disk::PageHeader page_header;
  std::vector<byte> body;
  DiagnosticRecord diagnostic;
  if (!ReadPageHeaderAndBody(
          request.device, request.page_size,
          kDatabaseLocalPrivateSecurityRootPageNumberV1, &page_header, &body,
          &diagnostic)) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorInitializeResultV1>(
        diagnostic.status, std::move(diagnostic));
  }
  if (page_header.page_type != disk::PageType::security_root ||
      page_header.page_number !=
          kDatabaseLocalPrivateSecurityRootPageNumberV1 ||
      page_header.database_uuid != request.database_uuid.value ||
      page_header.page_generation == 0 || !AllZero(body)) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInitializeResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.initialize_page_invalid",
        "fresh_security_root_not_exactly_empty");
  }
  const TypedUuid relation_uuid = SecurityRelationUuid();
  if (!relation_uuid.valid()) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorInitializeResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.relation_invalid");
  }

  DatabaseLocalPrivateSecurityLocatorInitializeResultV1 result;
  result.locator.slot_ordinal = 0;
  result.locator.lineage =
      DatabaseLocalPrivateSecurityLocatorLineageV1::fresh_bootstrap;
  result.locator.database_uuid = request.database_uuid.value;
  result.locator.filespace_uuid = page_header.filespace_uuid;
  result.locator.relation_uuid = relation_uuid.value;
  result.locator.relation_generation =
      kDatabaseLocalPrivateSecurityRelationGenerationV1;
  result.locator.locator_generation = 1;
  result.locator.security_context_generation =
      request.bootstrap_security_context_generation;
  const auto locator_bytes =
      EncodeDatabaseLocalPrivateSecurityLocatorV1(result.locator);
  result.locator.record_sha256 = LoadDigest(locator_bytes, 240);

  std::vector<byte> new_body(body.size(), 0);
  std::copy(locator_bytes.begin(), locator_bytes.end(),
            new_body.begin() +
                kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[0]);
  for (u32 index = 0; index < 2; ++index) {
    DatabaseLocalPrivateSecurityAnchorV1 anchor;
    anchor.copy_ordinal = index;
    anchor.flags = kAnchorPublishedFlags;
    anchor.database_uuid = request.database_uuid.value;
    anchor.relation_uuid = relation_uuid.value;
    anchor.relation_generation =
        kDatabaseLocalPrivateSecurityRelationGenerationV1;
    anchor.anchor_generation = 1;
    anchor.locator_slot = 0;
    anchor.locator_generation = 1;
    anchor.locator_sha256 = result.locator.record_sha256;
    const auto anchor_bytes = EncodeDatabaseLocalPrivateSecurityAnchorV1(anchor);
    std::copy(anchor_bytes.begin(), anchor_bytes.end(),
              new_body.begin() +
                  kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[index]);
  }
  const auto body_offset = disk::CheckDevicePageOffset(
      request.page_size, kDatabaseLocalPrivateSecurityRootPageNumberV1,
      disk::kPageHeaderSerializedBytes);
  if (!body_offset.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorInitializeResultV1>(
        body_offset.status, body_offset.diagnostic);
  }
  const auto written = request.device->WriteAt(
      body_offset.offset, new_body.data(), new_body.size());
  if (!written.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorInitializeResultV1>(
        written.status, written.diagnostic);
  }

  result.marker.flags = kMarkerFreshFlags;
  result.marker.database_uuid = request.database_uuid.value;
  result.marker.relation_uuid = relation_uuid.value;
  result.marker.relation_generation =
      kDatabaseLocalPrivateSecurityRelationGenerationV1;
  result.marker.migration_scan_count = 0;
  result.marker.locator_page_uuid = page_header.page_uuid;
  result.marker.locator_page_number =
      kDatabaseLocalPrivateSecurityRootPageNumberV1;
  result.marker.locator_page_generation = page_header.page_generation;
  result.marker.initial_locator_generation = 1;
  result.marker.initial_locator_sha256 = result.locator.record_sha256;
  result.marker_bytes =
      EncodeDatabaseLocalPrivateSecurityMarkerV1(result.marker);
  result.marker.marker_sha256 = LoadDigest(result.marker_bytes, 144);
  result.status = LocatorOkStatus();
  return result;
}

DatabaseLocalPrivateSecurityLocatorSuccessorResultV1
PublishDatabaseLocalPrivateSecurityLocatorSuccessorV1(
    const DatabaseLocalPrivateSecurityLocatorSuccessorRequestV1& request) {
  static constexpr std::array<std::string_view, 4> kFaultPoints = {
      "", "after_locator", "after_anchor_copy_0",
      "locator_readback_mismatch"};
  if (std::find(kFaultPoints.begin(), kFaultPoints.end(),
                request.fault_injection_point) == kFaultPoints.end() ||
      request.database_path.empty() ||
      request.expected_database_uuid.kind != UuidKind::database ||
      !request.expected_database_uuid.valid() ||
      !disk::IsSupportedDatabasePageSize(request.page_size) ||
      request.creator_transaction_uuid.kind != UuidKind::transaction ||
      !request.creator_transaction_uuid.valid() ||
      request.creator_local_transaction_id == 0 ||
      request.head_page_uuid.kind != UuidKind::page ||
      !request.head_page_uuid.valid() ||
      request.head_page_number <
          kDatabaseLocalPrivateSecurityFirstDataPageNumberV1 ||
      request.head_page_generation == 0 ||
      request.security_context_generation == 0 ||
      request.selected_prior.locator_generation == 0) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.successor_invalid",
        "successor_request_invalid");
  }

  const auto inventory =
      LoadLocalTransactionInventoryFromDatabase(request.database_path);
  if (!inventory.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        inventory.status, inventory.diagnostic);
  }
  const auto request_creator = mga::LookupLocalTransaction(
      inventory.inventory,
      mga::MakeLocalTransactionId(request.creator_local_transaction_id));
  if (!request_creator.ok() ||
      request_creator.entry.state != mga::TransactionState::active ||
      request_creator.entry.identity.transaction_uuid.kind !=
          UuidKind::transaction ||
      request_creator.entry.identity.transaction_uuid.kind !=
          request.creator_transaction_uuid.kind ||
      request_creator.entry.identity.transaction_uuid.value !=
          request.creator_transaction_uuid.value) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.successor_creator_invalid",
        "exact_active_inventory_creator_required");
  }

  disk::FileDevice device;
  const auto opened =
      device.Open(request.database_path, disk::FileOpenMode::open_existing);
  if (!opened.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        opened.status, opened.diagnostic);
  }
  DatabaseLocalPrivateSecurityLocatorInspectRequestV1 inspect_request;
  inspect_request.device = &device;
  inspect_request.marker_bytes = request.marker_bytes;
  inspect_request.expected_database_uuid = request.expected_database_uuid;
  inspect_request.page_size = request.page_size;
  const auto inspected =
      InspectDatabaseLocalPrivateSecurityLocatorV1(inspect_request);
  if (!inspected.ok() ||
      inspected.classification !=
          DatabaseLocalPrivateSecurityLocatorClassV1::sealed_current) {
    return inspected.ok()
               ? Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
                     kDatabaseLocalPrivateSecurityLocatorInvalid,
                     "storage.database_local_private_security_locator.successor_unsealed")
               : Propagate<
                     DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
                     inspected.status, inspected.diagnostic);
  }

  // Direct callers receive the same writer-lease and malformed-candidate
  // protection as the event-store preflight. Only a byte-zero inactive slot,
  // the same active creator, or an exactly inventory-proven terminal slot is
  // reusable. A nonzero malformed slot is never overwritten.
  for (u32 index = 0; index < 2; ++index) {
    if (inspected.locator_zero[index]) continue;
    if (!inspected.locator_shape_valid[index] ||
        !inspected.locator_digest_valid[index] ||
        !inspected.locator_context_valid[index] ||
        !LocatorCanonicalDigestValid(inspected.locators[index])) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.candidate_malformed",
          "nonzero_malformed_locator_candidate");
    }
    const auto& slot_locator = inspected.locators[index];
    if (slot_locator.locator_generation <= 1) continue;
    const auto slot_creator = mga::LookupLocalTransaction(
        inventory.inventory,
        mga::MakeLocalTransactionId(
            slot_locator.creator_local_transaction_id));
    if (!slot_creator.ok() ||
        slot_creator.entry.identity.transaction_uuid.kind !=
            UuidKind::transaction ||
        slot_creator.entry.identity.transaction_uuid.value !=
            slot_locator.creator_transaction_uuid) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.candidate_creator_invalid",
          "candidate_creator_inventory_identity_missing");
    }
    const auto slot_state = slot_creator.entry.state;
    if (slot_state == mga::TransactionState::active) {
      if (slot_locator.creator_local_transaction_id !=
              request.creator_local_transaction_id ||
          slot_locator.creator_transaction_uuid !=
              request.creator_transaction_uuid.value) {
        return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
            kDatabaseLocalPrivateSecurityLocatorInvalid,
            "storage.database_local_private_security_locator.concurrent_writer",
            "another_private_security_writer_active");
      }
      continue;
    }
    const bool final_state =
        slot_state == mga::TransactionState::committed ||
        slot_state == mga::TransactionState::archived ||
        slot_state == mga::TransactionState::rolled_back ||
        slot_state == mga::TransactionState::failed_terminal;
    if (!final_state) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.concurrent_writer",
          "private_security_candidate_creator_not_terminal");
    }
  }
  u32 selected_slot = 2;
  for (u32 index = 0; index < 2; ++index) {
    if (inspected.locator_context_valid[index] &&
        LocatorCanonicalDigestValid(inspected.locators[index]) &&
        LocatorFullFieldsEqual(inspected.locators[index],
                               request.selected_prior)) {
      selected_slot = index;
    }
  }
  if (selected_slot >= 2) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.successor_prior_invalid",
        "selected_prior_not_exact_decoded_locator");
  }
  const auto& selected_prior = inspected.locators[selected_slot];
  const u32 anchored_slot = inspected.anchored_locator_slot;
  const auto& anchored = inspected.locators[anchored_slot];
  bool same_transaction_staged_candidate = false;
  bool terminal_candidate_reuse = false;
  if (anchored.locator_generation > 1) {
    const auto anchored_creator = mga::LookupLocalTransaction(
        inventory.inventory,
        mga::MakeLocalTransactionId(
            anchored.creator_local_transaction_id));
    if (!anchored_creator.ok() ||
        anchored_creator.entry.identity.transaction_uuid.kind !=
            UuidKind::transaction ||
        anchored_creator.entry.identity.transaction_uuid.value !=
            anchored.creator_transaction_uuid) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.anchored_creator_invalid",
          "anchored_creator_inventory_identity_missing");
    }
    if (anchored_creator.entry.state == mga::TransactionState::active) {
      if (anchored.creator_local_transaction_id !=
              request.creator_local_transaction_id ||
          anchored.creator_transaction_uuid !=
              request.creator_transaction_uuid.value) {
        return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
            kDatabaseLocalPrivateSecurityLocatorInvalid,
            "storage.database_local_private_security_locator.concurrent_writer",
            "another_private_security_writer_active");
      }
      same_transaction_staged_candidate = true;
    } else if (anchored_creator.entry.state ==
                   mga::TransactionState::rolled_back ||
               anchored_creator.entry.state ==
                   mga::TransactionState::failed_terminal) {
      terminal_candidate_reuse = true;
    } else if (anchored_creator.entry.state !=
                   mga::TransactionState::committed &&
               anchored_creator.entry.state !=
                   mga::TransactionState::archived) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.anchored_creator_not_final",
          "anchored_creator_state_not_admitted");
    }
  }
  if (same_transaction_staged_candidate && selected_slot != anchored_slot) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.staged_candidate_mismatch",
        "same_creator_must_extend_anchored_candidate");
  }
  if (!same_transaction_staged_candidate && !terminal_candidate_reuse &&
      selected_slot != anchored_slot) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.committed_prior_mismatch",
        "committed_anchored_locator_required");
  }
  if (terminal_candidate_reuse && selected_slot == anchored_slot) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.terminal_candidate_selected",
        "rolled_back_or_failed_candidate_not_extendable");
  }

  u32 committed_slot = selected_slot;
  if (same_transaction_staged_candidate) {
    committed_slot = anchored_slot == 0 ? 1 : 0;
    if (!inspected.locator_context_valid[committed_slot] ||
        !LocatorCanonicalDigestValid(inspected.locators[committed_slot]) ||
        inspected.locators[committed_slot].record_sha256 !=
            anchored.prior_locator_sha256) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.staged_committed_prior_invalid",
          "same_transaction_committed_prior_missing");
    }
  }
  const auto& committed_prior = inspected.locators[committed_slot];
  if (committed_prior.locator_generation ==
          std::numeric_limits<u64>::max() ||
      selected_prior.security_context_generation ==
          std::numeric_limits<u64>::max() ||
      selected_prior.security_context_generation + 1 !=
          request.security_context_generation ||
      request.expected_predecessor_page_number !=
          selected_prior.head_page_number ||
      (same_transaction_staged_candidate &&
       (selected_prior.locator_generation !=
            committed_prior.locator_generation + 1 ||
        selected_prior.creator_local_transaction_id !=
            request.creator_local_transaction_id ||
        selected_prior.creator_transaction_uuid !=
            request.creator_transaction_uuid.value))) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.successor_prior_invalid",
        "selected_prior_or_generation_mismatch");
  }

  u64 anchor_generation = 0;
  for (u32 index = 0; index < 2; ++index) {
    if (inspected.anchor_valid[index]) {
      anchor_generation =
          std::max(anchor_generation,
                   inspected.anchors[index].anchor_generation);
    }
  }
  const bool anchors_on_prior =
      inspected.anchors[0].locator_sha256 ==
          committed_prior.record_sha256 &&
      inspected.anchors[1].locator_sha256 ==
          committed_prior.record_sha256 &&
      inspected.anchor_valid[0] && inspected.anchor_valid[1];
  if (!anchors_on_prior) {
    if (anchor_generation == std::numeric_limits<u64>::max()) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.anchor_generation_exhausted");
    }
    ++anchor_generation;
    for (u32 index = 0; index < 2; ++index) {
      DatabaseLocalPrivateSecurityAnchorV1 anchor;
      anchor.copy_ordinal = index;
      anchor.flags = kAnchorPublishedFlags;
      anchor.database_uuid = committed_prior.database_uuid;
      anchor.relation_uuid = committed_prior.relation_uuid;
      anchor.relation_generation = committed_prior.relation_generation;
      anchor.anchor_generation = anchor_generation;
      anchor.locator_slot = committed_slot;
      anchor.locator_generation =
          committed_prior.locator_generation;
      anchor.locator_sha256 = committed_prior.record_sha256;
      const auto encoded = EncodeDatabaseLocalPrivateSecurityAnchorV1(anchor);
      DiagnosticRecord diagnostic;
      if (!WriteLocatorPageRecord(
              &device, request.page_size,
              kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[index], encoded,
              &diagnostic)) {
        return Propagate<
            DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
            diagnostic.status, std::move(diagnostic));
      }
    }
  }

  disk::PageHeader head_header;
  std::vector<byte> head_body_bytes;
  DiagnosticRecord head_diagnostic;
  if (!ReadPageHeaderAndBody(&device, request.page_size,
                             request.head_page_number, &head_header,
                             &head_body_bytes, &head_diagnostic)) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        head_diagnostic.status, std::move(head_diagnostic));
  }
  const auto head_body = page::ParseRowDataPageBody(
      head_body_bytes, request.head_page_number);
  if (!head_body.ok() || head_header.page_type != disk::PageType::row_data ||
      head_header.database_uuid != request.expected_database_uuid.value ||
      head_header.filespace_uuid != selected_prior.filespace_uuid ||
      head_header.page_uuid != request.head_page_uuid.value ||
      head_header.page_generation != request.head_page_generation ||
      head_body.body.next_page_number !=
          request.expected_predecessor_page_number ||
      head_body.body.page_generation != request.head_page_generation ||
      head_body.body.relation_uuid.kind != UuidKind::object ||
      head_body.body.relation_uuid.value != selected_prior.relation_uuid) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.head_invalid",
        "written_head_readback_identity_or_reverse_link_invalid");
  }

  const u32 candidate_slot = same_transaction_staged_candidate
                                 ? anchored_slot
                                 : (committed_slot == 0 ? 1 : 0);
  DatabaseLocalPrivateSecurityLocatorV1 candidate;
  candidate.slot_ordinal = candidate_slot;
  candidate.lineage = committed_prior.lineage;
  candidate.database_uuid = committed_prior.database_uuid;
  candidate.filespace_uuid = committed_prior.filespace_uuid;
  candidate.relation_uuid = committed_prior.relation_uuid;
  candidate.relation_generation = committed_prior.relation_generation;
  candidate.locator_generation = committed_prior.locator_generation + 1;
  candidate.creator_transaction_uuid =
      request.creator_transaction_uuid.value;
  candidate.creator_local_transaction_id =
      request.creator_local_transaction_id;
  candidate.head_page_uuid = request.head_page_uuid.value;
  candidate.head_page_number = request.head_page_number;
  candidate.head_page_generation = request.head_page_generation;
  candidate.tail_page_uuid =
      selected_prior.chain_page_count == 0
          ? request.head_page_uuid.value
          : selected_prior.tail_page_uuid;
  candidate.tail_page_number =
      selected_prior.chain_page_count == 0
          ? request.head_page_number
          : selected_prior.tail_page_number;
  candidate.tail_page_generation =
      selected_prior.chain_page_count == 0
          ? request.head_page_generation
          : selected_prior.tail_page_generation;
  if (selected_prior.chain_page_count ==
      std::numeric_limits<u64>::max()) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.chain_count_exhausted");
  }
  candidate.chain_page_count = selected_prior.chain_page_count + 1;
  candidate.extent_min_page =
      selected_prior.chain_page_count == 0
          ? request.head_page_number
          : std::min(selected_prior.extent_min_page,
                     request.head_page_number);
  candidate.extent_max_page =
      selected_prior.chain_page_count == 0
          ? request.head_page_number
          : std::max(selected_prior.extent_max_page,
                     request.head_page_number);
  candidate.security_context_generation = request.security_context_generation;
  candidate.prior_locator_sha256 = committed_prior.record_sha256;
  const auto locator_bytes =
      EncodeDatabaseLocalPrivateSecurityLocatorV1(candidate);
  candidate.record_sha256 = LoadDigest(locator_bytes, 240);
  DiagnosticRecord write_diagnostic;
  if (!WriteLocatorPageRecord(
          &device, request.page_size,
          kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[candidate_slot],
          locator_bytes, &write_diagnostic,
          request.fault_injection_point == "locator_readback_mismatch")) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        write_diagnostic.status, std::move(write_diagnostic));
  }
  if (request.fault_injection_point == "after_locator") {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorWriteFailed,
        "storage.database_local_private_security_locator.fault_injected",
        "after_locator");
  }
  if (anchor_generation == std::numeric_limits<u64>::max()) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.anchor_generation_exhausted");
  }
  ++anchor_generation;
  for (u32 index = 0; index < 2; ++index) {
    DatabaseLocalPrivateSecurityAnchorV1 anchor;
    anchor.copy_ordinal = index;
    anchor.flags = kAnchorPublishedFlags;
    anchor.database_uuid = candidate.database_uuid;
    anchor.relation_uuid = candidate.relation_uuid;
    anchor.relation_generation = candidate.relation_generation;
    anchor.anchor_generation = anchor_generation;
    anchor.locator_slot = candidate_slot;
    anchor.locator_generation = candidate.locator_generation;
    anchor.locator_sha256 = candidate.record_sha256;
    const auto encoded = EncodeDatabaseLocalPrivateSecurityAnchorV1(anchor);
    if (!WriteLocatorPageRecord(
            &device, request.page_size,
            kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[index], encoded,
            &write_diagnostic)) {
      return Propagate<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          write_diagnostic.status, std::move(write_diagnostic));
    }
    if (index == 0 &&
        request.fault_injection_point == "after_anchor_copy_0") {
      return Refuse<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
          kDatabaseLocalPrivateSecurityLocatorWriteFailed,
          "storage.database_local_private_security_locator.fault_injected",
          "after_anchor_copy_0");
    }
  }
  const auto closed = device.Close();
  if (!closed.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorSuccessorResultV1>(
        closed.status, closed.diagnostic);
  }
  DatabaseLocalPrivateSecurityLocatorSuccessorResultV1 result;
  result.status = LocatorOkStatus();
  result.locator = candidate;
  result.anchor_generation = anchor_generation;
  result.metrics.locator_page_reads = inspected.metrics.locator_page_reads;
  result.metrics.security_chain_page_reads = 1;
  result.metrics.locator_migration_count =
      inspected.metrics.locator_migration_count;
  return result;
}

DatabaseLocalPrivateSecurityLocatorMigrationResultV1
MigrateLegacyDatabaseLocalPrivateSecurityLocatorInTemporaryImageV1(
    const DatabaseLocalPrivateSecurityLocatorMigrationRequestV1& request) {
  if (request.temporary_image == nullptr ||
      !request.temporary_image->is_open() ||
      request.temporary_image->read_only() ||
      request.database_uuid.kind != UuidKind::database ||
      !request.database_uuid.valid() ||
      !disk::IsSupportedDatabasePageSize(request.page_size) ||
      request.page_size <= disk::kPageHeaderSerializedBytes + 880 ||
      request.bootstrap_security_context_generation == 0) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.migration_request_invalid");
  }

  DatabaseLocalPrivateSecurityLocatorInspectRequestV1 legacy_request;
  legacy_request.device = request.temporary_image;
  legacy_request.expected_database_uuid = request.database_uuid;
  legacy_request.page_size = request.page_size;
  legacy_request.admit_exact_legacy_absence = true;
  const auto legacy =
      InspectDatabaseLocalPrivateSecurityLocatorV1(legacy_request);
  if (!legacy.ok() ||
      legacy.classification !=
          DatabaseLocalPrivateSecurityLocatorClassV1::exact_legacy_absence) {
    return legacy.ok()
               ? Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
                     kDatabaseLocalPrivateSecurityLocatorInvalid,
                     "storage.database_local_private_security_locator.migration_not_legacy")
               : Propagate<
                     DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
                     legacy.status, legacy.diagnostic);
  }
  const auto size = request.temporary_image->Size();
  if (!size.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        size.status, size.diagnostic);
  }
  if (size.size_bytes == 0 || size.size_bytes % request.page_size != 0) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.migration_extent_invalid",
        "temporary_image_size_not_page_aligned");
  }
  const u64 page_count = size.size_bytes / request.page_size;
  if (page_count <= kDatabaseLocalPrivateSecurityRootPageNumberV1) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.migration_extent_invalid",
        "temporary_image_core_pages_missing");
  }
  const TypedUuid relation_uuid = SecurityRelationUuid();
  if (!relation_uuid.valid()) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.relation_invalid");
  }
  const auto inventory = LoadLocalTransactionInventoryFromOpenDevice(
      request.temporary_image, request.page_size);
  if (!inventory.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        inventory.status, inventory.diagnostic);
  }
  disk::PageHeader inventory_page_header;
  std::vector<byte> inventory_page_body;
  DiagnosticRecord inventory_page_diagnostic;
  if (!ReadPageHeaderAndBody(
          request.temporary_image, request.page_size,
          kTransactionInventoryPageNumber, &inventory_page_header,
          &inventory_page_body, &inventory_page_diagnostic)) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        inventory_page_diagnostic.status,
        std::move(inventory_page_diagnostic));
  }
  if (inventory_page_header.page_type !=
          disk::PageType::transaction_inventory ||
      inventory_page_header.database_uuid != request.database_uuid.value ||
      inventory_page_header.filespace_uuid !=
          legacy.locator_page_header.filespace_uuid) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.migration_inventory_filespace_invalid");
  }

  std::vector<LegacySecurityPage> security_pages;
  security_pages.reserve(16);
  for (u64 page_number = 1; page_number < page_count; ++page_number) {
    disk::SerializedPageHeader raw_header{};
    std::vector<byte> raw_body;
    DiagnosticRecord io_diagnostic;
    if (!ReadRawPage(request.temporary_image, request.page_size, page_number,
                     &raw_header, &raw_body, &io_diagnostic)) {
      return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          io_diagnostic.status, std::move(io_diagnostic));
    }
    const bool claims_security = RawBodyClaimsSecurity(
        raw_body, request.database_uuid.value, relation_uuid.value);
    const auto parsed_header = disk::ParsePageHeader(raw_header);
    if (!parsed_header.ok()) {
      if (claims_security) {
        return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
            kDatabaseLocalPrivateSecurityLocatorInvalid,
            "storage.database_local_private_security_locator.migration_security_header_invalid",
            std::to_string(page_number));
      }
      continue;
    }
    if (parsed_header.header.page_type != disk::PageType::row_data) {
      if (claims_security && page_number !=
                                 kDatabaseLocalPrivateSecurityRootPageNumberV1) {
        return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
            kDatabaseLocalPrivateSecurityLocatorInvalid,
            "storage.database_local_private_security_locator.migration_security_type_invalid",
            std::to_string(page_number));
      }
      continue;
    }
    const auto parsed_body =
        page::ParseRowDataPageBody(raw_body, page_number);
    if (!parsed_body.ok()) {
      if (claims_security) {
        return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
            kDatabaseLocalPrivateSecurityLocatorInvalid,
            "storage.database_local_private_security_locator.migration_security_body_invalid",
            std::to_string(page_number));
      }
      continue;
    }
    if (parsed_body.body.relation_uuid.kind != UuidKind::object ||
        parsed_body.body.relation_uuid.value != relation_uuid.value) {
      if (claims_security) {
        return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
            kDatabaseLocalPrivateSecurityLocatorInvalid,
            "storage.database_local_private_security_locator.migration_security_relation_mismatch",
            std::to_string(page_number));
      }
      continue;
    }
    if (parsed_header.header.database_uuid != request.database_uuid.value ||
        parsed_header.header.filespace_uuid !=
            legacy.locator_page_header.filespace_uuid ||
        parsed_header.header.page_number != page_number ||
        parsed_header.header.page_size != request.page_size ||
        parsed_header.header.page_generation == 0 ||
        parsed_body.body.page_number != page_number ||
        parsed_body.body.page_generation !=
            parsed_header.header.page_generation ||
        parsed_body.body.segment_id != 1 ||
        parsed_body.body.segment_generation != 1 ||
        parsed_body.body.compaction_generation != 1 ||
        parsed_body.body.next_page_number != 0 ||
        parsed_body.body.rows.size() != 1 ||
        parsed_body.body.slots.size() != 1 ||
        parsed_body.body.rows[0].deleted ||
        parsed_body.body.rows[0].row_uuid.kind != UuidKind::row ||
        !parsed_body.body.rows[0].row_uuid.valid() ||
        parsed_body.body.rows[0].transaction_uuid.kind !=
            UuidKind::transaction ||
        !parsed_body.body.rows[0].transaction_uuid.valid() ||
        parsed_body.body.rows[0].internal_row_ordinal != 1 ||
        parsed_body.body.rows[0].stable_slot_id != 1 ||
        parsed_body.body.rows[0].row_version != 1 ||
        parsed_body.body.rows[0].previous_row_version != 0 ||
        parsed_body.body.rows[0].next_row_version != 0 ||
        parsed_body.body.slots[0].stable_slot_id != 1 ||
        parsed_body.body.slots[0].deleted ||
        parsed_body.body.rows[0].cells.size() != 1 ||
        parsed_body.body.rows[0].cells[0].column_ordinal != 1 ||
        parsed_body.body.rows[0].cells[0].value.type_id !=
            scratchbird::core::datatypes::CanonicalTypeId::binary ||
        parsed_body.body.rows[0].cells[0].value.is_null ||
        parsed_body.body.rows[0].cells[0].value.payload_is_toast_reference) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.migration_security_page_shape_invalid",
          std::to_string(page_number));
    }
    LegacySecurityPage security_page;
    security_page.header = parsed_header.header;
    security_page.body = parsed_body.body;
    const auto& row = security_page.body.rows[0];
    if (!DecodeLegacySecurityBatch(row.cells[0].value.payload,
                                   &security_page.batch) ||
        security_page.batch.database_uuid != request.database_uuid.value ||
        security_page.batch.relation_uuid != relation_uuid.value ||
        row.local_transaction_id !=
            security_page.batch.creator_local_transaction_id ||
        row.transaction_uuid.kind != UuidKind::transaction ||
        row.transaction_uuid.value != security_page.batch.transaction_uuid) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.migration_batch_invalid",
          std::to_string(page_number));
    }
    const auto creator = mga::LookupLocalTransaction(
        inventory.inventory,
        mga::MakeLocalTransactionId(
            security_page.batch.creator_local_transaction_id));
    if (!creator.ok() ||
        creator.entry.identity.transaction_uuid.kind != UuidKind::transaction ||
        creator.entry.identity.transaction_uuid.value !=
            security_page.batch.transaction_uuid) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.migration_inventory_identity_invalid",
          std::to_string(page_number));
    }
    if (creator.entry.state == mga::TransactionState::committed ||
        creator.entry.state == mga::TransactionState::archived) {
      security_page.committed_visible = true;
    } else if (creator.entry.state == mga::TransactionState::rolled_back ||
               creator.entry.state == mga::TransactionState::failed_terminal) {
      security_page.committed_visible = false;
    } else {
      return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.migration_inventory_not_final",
          std::to_string(page_number));
    }
    security_pages.push_back(std::move(security_page));
  }

  std::map<u64, std::size_t> committed_by_successor;
  for (std::size_t index = 0; index < security_pages.size(); ++index) {
    if (!security_pages[index].committed_visible) continue;
    const u64 generation =
        security_pages[index].batch.successor_security_context_generation;
    if (!committed_by_successor.emplace(generation, index).second) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.migration_generation_duplicate",
          std::to_string(generation));
    }
  }
  u64 expected_generation = request.bootstrap_security_context_generation;
  for (const auto& entry : committed_by_successor) {
    if (expected_generation == std::numeric_limits<u64>::max() ||
        entry.first != expected_generation + 1 ||
        security_pages[entry.second]
                .batch.prior_security_context_generation !=
            expected_generation) {
      return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          kDatabaseLocalPrivateSecurityLocatorInvalid,
          "storage.database_local_private_security_locator.migration_generation_chain_invalid",
          std::to_string(entry.first));
    }
    expected_generation = entry.first;
  }

  for (auto& security_page : security_pages) {
    const disk::PageHeader* predecessor = nullptr;
    if (security_page.batch.prior_security_context_generation !=
        request.bootstrap_security_context_generation) {
      const auto found = committed_by_successor.find(
          security_page.batch.prior_security_context_generation);
      if (found == committed_by_successor.end()) {
        return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
            kDatabaseLocalPrivateSecurityLocatorInvalid,
            "storage.database_local_private_security_locator.migration_predecessor_missing",
            std::to_string(security_page.header.page_number));
      }
      predecessor = &security_pages[found->second].header;
    }
    DiagnosticRecord write_diagnostic;
    if (!WriteMigratedSecurityPage(request.temporary_image, request.page_size,
                                   &security_page, predecessor,
                                   &write_diagnostic)) {
      if (!write_diagnostic.diagnostic_code.empty()) {
        return Propagate<
            DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
            write_diagnostic.status, std::move(write_diagnostic));
      }
      return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
          kDatabaseLocalPrivateSecurityLocatorWriteFailed,
          "storage.database_local_private_security_locator.migration_page_write_failed",
          std::to_string(security_page.header.page_number));
    }
  }

  disk::PageHeader security_root_header = legacy.locator_page_header;
  security_root_header.page_type = disk::PageType::security_root;
  security_root_header.flags = 0;
  const auto serialized_root = disk::SerializePageHeader(security_root_header);
  if (!serialized_root.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        serialized_root.status, serialized_root.diagnostic);
  }
  const auto root_offset = disk::CheckDevicePageOffset(
      request.page_size, kDatabaseLocalPrivateSecurityRootPageNumberV1);
  if (!root_offset.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        root_offset.status, root_offset.diagnostic);
  }
  const auto root_header_write = request.temporary_image->WriteAt(
      root_offset.offset, serialized_root.serialized.data(),
      serialized_root.serialized.size());
  if (!root_header_write.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        root_header_write.status, root_header_write.diagnostic);
  }

  DatabaseLocalPrivateSecurityLocatorMigrationResultV1 result;
  result.locator.slot_ordinal = 0;
  result.locator.lineage =
      DatabaseLocalPrivateSecurityLocatorLineageV1::sealed_legacy_migration;
  result.locator.database_uuid = request.database_uuid.value;
  result.locator.filespace_uuid = security_root_header.filespace_uuid;
  result.locator.relation_uuid = relation_uuid.value;
  result.locator.relation_generation =
      kDatabaseLocalPrivateSecurityRelationGenerationV1;
  result.locator.locator_generation = 1;
  result.locator.security_context_generation = expected_generation;
  if (!committed_by_successor.empty()) {
    const auto& tail = security_pages[committed_by_successor.begin()->second];
    const auto& head = security_pages[committed_by_successor.rbegin()->second];
    result.locator.head_page_uuid = head.header.page_uuid;
    result.locator.head_page_number = head.header.page_number;
    result.locator.head_page_generation = head.header.page_generation;
    result.locator.tail_page_uuid = tail.header.page_uuid;
    result.locator.tail_page_number = tail.header.page_number;
    result.locator.tail_page_generation = tail.header.page_generation;
    result.locator.chain_page_count = committed_by_successor.size();
    result.locator.extent_min_page = std::numeric_limits<u64>::max();
    result.locator.extent_max_page = 0;
    for (const auto& entry : committed_by_successor) {
      const u64 page_number = security_pages[entry.second].header.page_number;
      result.locator.extent_min_page =
          std::min(result.locator.extent_min_page, page_number);
      result.locator.extent_max_page =
          std::max(result.locator.extent_max_page, page_number);
    }
  }
  const auto locator_bytes =
      EncodeDatabaseLocalPrivateSecurityLocatorV1(result.locator);
  result.locator.record_sha256 = LoadDigest(locator_bytes, 240);
  std::vector<byte> root_body(
      request.page_size - disk::kPageHeaderSerializedBytes, 0);
  std::copy(locator_bytes.begin(), locator_bytes.end(),
            root_body.begin() +
                kDatabaseLocalPrivateSecurityLocatorBodyOffsetsV1[0]);
  for (u32 index = 0; index < 2; ++index) {
    DatabaseLocalPrivateSecurityAnchorV1 anchor;
    anchor.copy_ordinal = index;
    anchor.flags = kAnchorPublishedFlags;
    anchor.database_uuid = request.database_uuid.value;
    anchor.relation_uuid = relation_uuid.value;
    anchor.relation_generation =
        kDatabaseLocalPrivateSecurityRelationGenerationV1;
    anchor.anchor_generation = 1;
    anchor.locator_slot = 0;
    anchor.locator_generation = 1;
    anchor.locator_sha256 = result.locator.record_sha256;
    const auto anchor_bytes = EncodeDatabaseLocalPrivateSecurityAnchorV1(anchor);
    std::copy(anchor_bytes.begin(), anchor_bytes.end(),
              root_body.begin() +
                  kDatabaseLocalPrivateSecurityAnchorBodyOffsetsV1[index]);
  }
  const auto root_body_offset = disk::CheckDevicePageOffset(
      request.page_size, kDatabaseLocalPrivateSecurityRootPageNumberV1,
      disk::kPageHeaderSerializedBytes);
  if (!root_body_offset.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        root_body_offset.status, root_body_offset.diagnostic);
  }
  const auto root_body_write = request.temporary_image->WriteAt(
      root_body_offset.offset, root_body.data(), root_body.size());
  if (!root_body_write.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        root_body_write.status, root_body_write.diagnostic);
  }

  result.marker.flags = kMarkerMigratedFlags;
  result.marker.database_uuid = request.database_uuid.value;
  result.marker.relation_uuid = relation_uuid.value;
  result.marker.relation_generation =
      kDatabaseLocalPrivateSecurityRelationGenerationV1;
  result.marker.migration_scan_count = 1;
  result.marker.locator_page_uuid = security_root_header.page_uuid;
  result.marker.locator_page_number =
      kDatabaseLocalPrivateSecurityRootPageNumberV1;
  result.marker.locator_page_generation = security_root_header.page_generation;
  result.marker.initial_locator_generation = 1;
  result.marker.initial_locator_sha256 = result.locator.record_sha256;
  result.marker_bytes =
      EncodeDatabaseLocalPrivateSecurityMarkerV1(result.marker);
  result.marker.marker_sha256 = LoadDigest(result.marker_bytes, 144);
  const auto sync = request.temporary_image->Sync();
  if (!sync.ok()) {
    return Propagate<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        sync.status, sync.diagnostic);
  }
  DatabaseLocalPrivateSecurityLocatorInspectRequestV1 readback_request;
  readback_request.device = request.temporary_image;
  readback_request.marker_bytes = result.marker_bytes;
  readback_request.expected_database_uuid = request.database_uuid;
  readback_request.page_size = request.page_size;
  const auto readback =
      InspectDatabaseLocalPrivateSecurityLocatorV1(readback_request);
  if (!readback.ok() ||
      readback.classification !=
          DatabaseLocalPrivateSecurityLocatorClassV1::sealed_current ||
      !LocatorFullFieldsEqual(
          readback.locators[readback.anchored_locator_slot],
          result.locator)) {
    return readback.ok()
               ? Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
                     kDatabaseLocalPrivateSecurityLocatorInvalid,
                     "storage.database_local_private_security_locator.migration_readback_invalid")
               : Propagate<
                     DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
                     readback.status, readback.diagnostic);
  }
  std::string chain_readback_refusal;
  if (!ValidateMigratedSecurityChainReadback(
          request.temporary_image, request.page_size,
          request.database_uuid.value, security_root_header.filespace_uuid,
          relation_uuid.value, request.bootstrap_security_context_generation,
          result.locator, inventory.inventory, &chain_readback_refusal)) {
    return Refuse<DatabaseLocalPrivateSecurityLocatorMigrationResultV1>(
        kDatabaseLocalPrivateSecurityLocatorInvalid,
        "storage.database_local_private_security_locator.migration_chain_readback_invalid",
        std::move(chain_readback_refusal));
  }
  result.status = LocatorOkStatus();
  result.metrics.locator_page_reads =
      legacy.metrics.locator_page_reads + readback.metrics.locator_page_reads;
  result.metrics.legacy_scan_page_reads = page_count;
  result.metrics.locator_migration_count = 1;
  return result;
}

DiagnosticRecord MakeDatabaseLocalPrivateSecurityLocatorDiagnosticV1(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) arguments.push_back({"detail", std::move(detail)});
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {},
                        "storage.database_local_private_security_locator");
}

}  // namespace scratchbird::storage::database
