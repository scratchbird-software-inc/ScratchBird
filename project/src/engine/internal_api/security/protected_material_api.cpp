// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/protected_material_api.hpp"

#include "api_diagnostics.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "runtime_platform.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
namespace core_hash = scratchbird::core::hash;
namespace disk = scratchbird::storage::disk;

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

// SEARCH_KEY: SB_ENGINE_SECURITY_PROTECTED_MATERIAL_DURABLE_CATALOG
inline constexpr std::array<byte, 8> kProtectedMaterialCatalogMagic = {
    'S', 'B', 'P', 'M', 'C', 'A', 'T', '1'};
inline constexpr std::array<byte, 8> kProtectedMaterialRecordMagic = {
    'S', 'B', 'P', 'M', 'R', 'E', 'C', '1'};
inline constexpr u16 kProtectedMaterialCatalogVersion = 1;
inline constexpr u16 kProtectedMaterialCatalogHeaderBytes = 80;
inline constexpr u16 kProtectedMaterialRecordHeaderBytes = 88;
inline constexpr u16 kProtectedMaterialDigestBytes = core_hash::kSha256DigestBytes;

inline constexpr u32 kCatalogOffsetVersion = 8;
inline constexpr u32 kCatalogOffsetHeaderBytes = 10;
inline constexpr u32 kCatalogOffsetFlags = 12;
inline constexpr u32 kCatalogOffsetGeneration = 16;
inline constexpr u32 kCatalogOffsetRecordCount = 24;
inline constexpr u32 kCatalogOffsetRecordBytes = 32;
inline constexpr u32 kCatalogOffsetDigestBytes = 40;
inline constexpr u32 kCatalogOffsetDigest = 44;

inline constexpr u32 kRecordOffsetVersion = 8;
inline constexpr u32 kRecordOffsetHeaderBytes = 10;
inline constexpr u32 kRecordOffsetKind = 12;
inline constexpr u32 kRecordOffsetFlags = 16;
inline constexpr u32 kRecordOffsetSequence = 24;
inline constexpr u32 kRecordOffsetPayloadBytes = 40;
inline constexpr u32 kRecordOffsetPayloadDigestBytes = 48;
inline constexpr u32 kRecordOffsetDigest = 52;

enum class ProtectedMaterialRecordKind : u16 {
  material = 1,
  version = 2,
  audit = 3,
};

struct ProtectedMaterialCatalogImage {
  u64 generation = 0;
  std::vector<EngineProtectedMaterialCatalogEntry> materials;
  std::vector<EngineProtectedMaterialVersionCatalogEntry> versions;
  std::vector<EngineProtectedMaterialAuditEvent> audit_events;
};

struct ProtectedMaterialCatalogLoadResult {
  bool ok = false;
  bool present = false;
  EngineApiDiagnostic diagnostic;
  ProtectedMaterialCatalogImage image;
};

std::vector<EngineProtectedMaterialCacheEntry>& Cache() {
  static std::vector<EngineProtectedMaterialCacheEntry> cache;
  return cache;
}

std::vector<EngineProtectedMaterialCatalogEntry>& MaterialCatalog() {
  static std::vector<EngineProtectedMaterialCatalogEntry> catalog;
  return catalog;
}

std::vector<EngineProtectedMaterialVersionCatalogEntry>& MaterialVersions() {
  static std::vector<EngineProtectedMaterialVersionCatalogEntry> versions;
  return versions;
}

std::vector<EngineProtectedMaterialAuditEvent>& MaterialAuditEvents() {
  static std::vector<EngineProtectedMaterialAuditEvent> events;
  return events;
}

std::mutex& CacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::uint64_t& GenerationCounter() {
  static std::uint64_t generation = 0;
  return generation;
}

std::uint64_t& MaterialCatalogGenerationCounter() {
  static std::uint64_t generation = 0;
  return generation;
}

bool MagicEquals(const std::vector<byte>& bytes,
                 std::size_t offset,
                 const std::array<byte, 8>& magic) {
  if (bytes.size() < offset + magic.size()) { return false; }
  return std::equal(magic.begin(),
                    magic.end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

void Store16(std::vector<byte>* out, std::size_t offset, u16 value) {
  StoreLittle16(out->data() + offset, value);
}

void Store32(std::vector<byte>* out, std::size_t offset, u32 value) {
  StoreLittle32(out->data() + offset, value);
}

void Store64(std::vector<byte>* out, std::size_t offset, u64 value) {
  StoreLittle64(out->data() + offset, value);
}

u16 Load16(const std::vector<byte>& bytes, std::size_t offset) {
  return LoadLittle16(bytes.data() + offset);
}

u32 Load32(const std::vector<byte>& bytes, std::size_t offset) {
  return LoadLittle32(bytes.data() + offset);
}

u64 Load64(const std::vector<byte>& bytes, std::size_t offset) {
  return LoadLittle64(bytes.data() + offset);
}

void Append32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  Store32(out, offset, value);
}

void Append64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  Store64(out, offset, value);
}

void AppendBool(std::vector<byte>* out, bool value) {
  out->push_back(value ? byte{1} : byte{0});
}

bool AppendLengthPrefixedString(std::vector<byte>* out, const std::string& value) {
  if (value.size() > std::numeric_limits<u32>::max()) { return false; }
  Append32(out, static_cast<u32>(value.size()));
  if (!value.empty()) {
    out->insert(out->end(),
                reinterpret_cast<const byte*>(value.data()),
                reinterpret_cast<const byte*>(value.data()) + value.size());
  }
  return true;
}

bool ReadLengthPrefixedString(const std::vector<byte>& payload,
                              std::size_t* offset,
                              std::string* out) {
  if (*offset > payload.size() || payload.size() - *offset < sizeof(u32)) { return false; }
  const u32 length = Load32(payload, *offset);
  *offset += sizeof(u32);
  if (payload.size() - *offset < length) { return false; }
  out->assign(reinterpret_cast<const char*>(payload.data() + *offset), length);
  *offset += length;
  return true;
}

bool ReadU64(const std::vector<byte>& payload, std::size_t* offset, u64* out) {
  if (*offset > payload.size() || payload.size() - *offset < sizeof(u64)) { return false; }
  *out = Load64(payload, *offset);
  *offset += sizeof(u64);
  return true;
}

bool ReadBool(const std::vector<byte>& payload, std::size_t* offset, bool* out) {
  if (*offset >= payload.size()) { return false; }
  const byte value = payload[*offset];
  *offset += 1;
  if (value != byte{0} && value != byte{1}) { return false; }
  *out = value == byte{1};
  return true;
}

std::vector<byte> DigestInputWithZeroedRange(std::vector<byte> encoded,
                                             std::size_t offset,
                                             std::size_t bytes) {
  if (encoded.size() >= offset + bytes) {
    std::fill(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
              encoded.begin() + static_cast<std::ptrdiff_t>(offset + bytes),
              byte{0});
  }
  return encoded;
}

bool Sha256DigestMatches(const std::vector<byte>& encoded,
                         std::size_t digest_offset,
                         std::size_t digest_bytes) {
  if (digest_bytes != kProtectedMaterialDigestBytes ||
      encoded.size() < digest_offset + digest_bytes) {
    return false;
  }
  const auto digest_input =
      DigestInputWithZeroedRange(encoded, digest_offset, digest_bytes);
  const auto computed = core_hash::ComputeSha256Digest(digest_input);
  if (!computed.ok()) { return false; }
  const std::vector<byte> stored(
      encoded.begin() + static_cast<std::ptrdiff_t>(digest_offset),
      encoded.begin() + static_cast<std::ptrdiff_t>(digest_offset + digest_bytes));
  return core_hash::ConstantTimeEqual(stored, core_hash::DigestVector(computed.digest));
}

bool AttachSha256Digest(std::vector<byte>* encoded,
                        std::size_t digest_offset,
                        std::size_t digest_bytes) {
  if (digest_bytes != kProtectedMaterialDigestBytes ||
      encoded->size() < digest_offset + digest_bytes) {
    return false;
  }
  std::fill(encoded->begin() + static_cast<std::ptrdiff_t>(digest_offset),
            encoded->begin() + static_cast<std::ptrdiff_t>(digest_offset + digest_bytes),
            byte{0});
  const auto computed = core_hash::ComputeSha256Digest(*encoded);
  if (!computed.ok()) { return false; }
  const auto digest = core_hash::DigestVector(computed.digest);
  std::copy(digest.begin(),
            digest.end(),
            encoded->begin() + static_cast<std::ptrdiff_t>(digest_offset));
  return true;
}

EngineApiDiagnostic ProtectedMaterialCatalogDiagnostic(const std::string& detail) {
  return MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.DURABLE_CATALOG_INVALID", detail);
}

ProtectedMaterialCatalogLoadResult ProtectedMaterialCatalogAbsent() {
  ProtectedMaterialCatalogLoadResult result;
  result.ok = true;
  result.present = false;
  return result;
}

ProtectedMaterialCatalogLoadResult ProtectedMaterialCatalogError(const std::string& detail) {
  ProtectedMaterialCatalogLoadResult result;
  result.ok = false;
  result.present = true;
  result.diagnostic = ProtectedMaterialCatalogDiagnostic(detail);
  return result;
}

std::string ProtectedMaterialCatalogPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.protected_material_catalog";
}

EngineApiDiagnostic ValidateProtectedMaterialCatalogPath(const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return ProtectedMaterialCatalogDiagnostic("protected_material_catalog_database_path_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

bool SerializePolicy(const EngineProtectedMaterialPolicySet& policy,
                     std::vector<byte>* payload) {
  if (!AppendLengthPrefixedString(payload, policy.retention_policy_uuid) ||
      !AppendLengthPrefixedString(payload, policy.access_policy_uuid) ||
      !AppendLengthPrefixedString(payload, policy.release_policy_uuid) ||
      !AppendLengthPrefixedString(payload, policy.purge_policy_uuid) ||
      !AppendLengthPrefixedString(payload, policy.audit_policy_uuid)) {
    return false;
  }
  Append64(payload, policy.retention_until_epoch_millis);
  AppendBool(payload, policy.legal_hold);
  if (policy.release_purposes.size() > std::numeric_limits<u32>::max()) { return false; }
  Append32(payload, static_cast<u32>(policy.release_purposes.size()));
  for (const auto& purpose : policy.release_purposes) {
    if (!AppendLengthPrefixedString(payload, purpose)) { return false; }
  }
  return true;
}

bool DeserializePolicy(const std::vector<byte>& payload,
                       std::size_t* offset,
                       EngineProtectedMaterialPolicySet* policy) {
  if (!ReadLengthPrefixedString(payload, offset, &policy->retention_policy_uuid) ||
      !ReadLengthPrefixedString(payload, offset, &policy->access_policy_uuid) ||
      !ReadLengthPrefixedString(payload, offset, &policy->release_policy_uuid) ||
      !ReadLengthPrefixedString(payload, offset, &policy->purge_policy_uuid) ||
      !ReadLengthPrefixedString(payload, offset, &policy->audit_policy_uuid) ||
      !ReadU64(payload, offset, &policy->retention_until_epoch_millis) ||
      !ReadBool(payload, offset, &policy->legal_hold)) {
    return false;
  }
  if (*offset > payload.size() || payload.size() - *offset < sizeof(u32)) { return false; }
  const u32 purpose_count = Load32(payload, *offset);
  *offset += sizeof(u32);
  policy->release_purposes.clear();
  policy->release_purposes.reserve(purpose_count);
  for (u32 i = 0; i < purpose_count; ++i) {
    std::string purpose;
    if (!ReadLengthPrefixedString(payload, offset, &purpose)) { return false; }
    policy->release_purposes.push_back(std::move(purpose));
  }
  return true;
}

bool SerializeMaterialPayload(const EngineProtectedMaterialCatalogEntry& material,
                              std::vector<byte>* payload) {
  if (!AppendLengthPrefixedString(payload, material.database_uuid) ||
      !AppendLengthPrefixedString(payload, material.protected_material_uuid) ||
      !AppendLengthPrefixedString(payload, material.object_class) ||
      !AppendLengthPrefixedString(payload, material.owner_scope_uuid) ||
      !AppendLengthPrefixedString(payload, material.purpose_class) ||
      !AppendLengthPrefixedString(payload, material.storage_class) ||
      !AppendLengthPrefixedString(payload, material.lifecycle_state) ||
      !AppendLengthPrefixedString(payload, material.active_version_uuid) ||
      !SerializePolicy(material.policy, payload)) {
    return false;
  }
  Append64(payload, material.catalog_generation_id);
  Append64(payload, material.created_local_transaction_id);
  Append64(payload, material.updated_local_transaction_id);
  Append64(payload, material.security_epoch);
  AppendBool(payload, material.purged);
  return true;
}

bool DeserializeMaterialPayload(const std::vector<byte>& payload,
                                EngineProtectedMaterialCatalogEntry* material) {
  std::size_t offset = 0;
  if (!ReadLengthPrefixedString(payload, &offset, &material->database_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &material->protected_material_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &material->object_class) ||
      !ReadLengthPrefixedString(payload, &offset, &material->owner_scope_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &material->purpose_class) ||
      !ReadLengthPrefixedString(payload, &offset, &material->storage_class) ||
      !ReadLengthPrefixedString(payload, &offset, &material->lifecycle_state) ||
      !ReadLengthPrefixedString(payload, &offset, &material->active_version_uuid) ||
      !DeserializePolicy(payload, &offset, &material->policy) ||
      !ReadU64(payload, &offset, &material->catalog_generation_id) ||
      !ReadU64(payload, &offset, &material->created_local_transaction_id) ||
      !ReadU64(payload, &offset, &material->updated_local_transaction_id) ||
      !ReadU64(payload, &offset, &material->security_epoch) ||
      !ReadBool(payload, &offset, &material->purged)) {
    return false;
  }
  return offset == payload.size();
}

bool SerializeVersionPayload(const EngineProtectedMaterialVersionCatalogEntry& version,
                             std::vector<byte>* payload) {
  if (!AppendLengthPrefixedString(payload, version.database_uuid) ||
      !AppendLengthPrefixedString(payload, version.protected_material_uuid) ||
      !AppendLengthPrefixedString(payload, version.protected_material_version_uuid) ||
      !AppendLengthPrefixedString(payload, version.protected_reference) ||
      !AppendLengthPrefixedString(payload, version.envelope_reference) ||
      !AppendLengthPrefixedString(payload, version.payload_hash) ||
      !AppendLengthPrefixedString(payload, version.storage_class) ||
      !AppendLengthPrefixedString(payload, version.rotation_state) ||
      !SerializePolicy(version.policy, payload)) {
    return false;
  }
  Append64(payload, version.version_number);
  Append64(payload, version.valid_from_local_transaction_id);
  Append64(payload, version.valid_until_local_transaction_id);
  Append64(payload, version.catalog_generation_id);
  Append64(payload, version.security_epoch);
  AppendBool(payload, version.active);
  AppendBool(payload, version.purged);
  return true;
}

bool DeserializeVersionPayload(const std::vector<byte>& payload,
                               EngineProtectedMaterialVersionCatalogEntry* version) {
  std::size_t offset = 0;
  if (!ReadLengthPrefixedString(payload, &offset, &version->database_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &version->protected_material_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &version->protected_material_version_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &version->protected_reference) ||
      !ReadLengthPrefixedString(payload, &offset, &version->envelope_reference) ||
      !ReadLengthPrefixedString(payload, &offset, &version->payload_hash) ||
      !ReadLengthPrefixedString(payload, &offset, &version->storage_class) ||
      !ReadLengthPrefixedString(payload, &offset, &version->rotation_state) ||
      !DeserializePolicy(payload, &offset, &version->policy) ||
      !ReadU64(payload, &offset, &version->version_number) ||
      !ReadU64(payload, &offset, &version->valid_from_local_transaction_id) ||
      !ReadU64(payload, &offset, &version->valid_until_local_transaction_id) ||
      !ReadU64(payload, &offset, &version->catalog_generation_id) ||
      !ReadU64(payload, &offset, &version->security_epoch) ||
      !ReadBool(payload, &offset, &version->active) ||
      !ReadBool(payload, &offset, &version->purged)) {
    return false;
  }
  return offset == payload.size();
}

bool SerializeAuditPayload(const EngineProtectedMaterialAuditEvent& event,
                           std::vector<byte>* payload) {
  if (!AppendLengthPrefixedString(payload, event.audit_event_uuid) ||
      !AppendLengthPrefixedString(payload, event.database_uuid) ||
      !AppendLengthPrefixedString(payload, event.protected_material_uuid) ||
      !AppendLengthPrefixedString(payload, event.protected_material_version_uuid) ||
      !AppendLengthPrefixedString(payload, event.actor_uuid) ||
      !AppendLengthPrefixedString(payload, event.event_kind) ||
      !AppendLengthPrefixedString(payload, event.decision) ||
      !AppendLengthPrefixedString(payload, event.diagnostic_code) ||
      !AppendLengthPrefixedString(payload, event.redacted_detail)) {
    return false;
  }
  Append64(payload, event.event_epoch_millis);
  Append64(payload, event.local_transaction_id);
  Append64(payload, event.catalog_generation_id);
  AppendBool(payload, event.redaction_applied);
  return true;
}

bool DeserializeAuditPayload(const std::vector<byte>& payload,
                             EngineProtectedMaterialAuditEvent* event) {
  std::size_t offset = 0;
  if (!ReadLengthPrefixedString(payload, &offset, &event->audit_event_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &event->database_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &event->protected_material_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &event->protected_material_version_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &event->actor_uuid) ||
      !ReadLengthPrefixedString(payload, &offset, &event->event_kind) ||
      !ReadLengthPrefixedString(payload, &offset, &event->decision) ||
      !ReadLengthPrefixedString(payload, &offset, &event->diagnostic_code) ||
      !ReadLengthPrefixedString(payload, &offset, &event->redacted_detail) ||
      !ReadU64(payload, &offset, &event->event_epoch_millis) ||
      !ReadU64(payload, &offset, &event->local_transaction_id) ||
      !ReadU64(payload, &offset, &event->catalog_generation_id) ||
      !ReadBool(payload, &offset, &event->redaction_applied)) {
    return false;
  }
  return offset == payload.size();
}

bool EncodeProtectedMaterialRecord(ProtectedMaterialRecordKind kind,
                                   u64 sequence,
                                   const std::vector<byte>& payload,
                                   std::vector<byte>* encoded) {
  if (payload.size() > std::numeric_limits<u64>::max()) { return false; }
  std::vector<byte> record(kProtectedMaterialRecordHeaderBytes);
  std::copy(kProtectedMaterialRecordMagic.begin(),
            kProtectedMaterialRecordMagic.end(),
            record.begin());
  Store16(&record, kRecordOffsetVersion, kProtectedMaterialCatalogVersion);
  Store16(&record, kRecordOffsetHeaderBytes, kProtectedMaterialRecordHeaderBytes);
  Store16(&record, kRecordOffsetKind, static_cast<u16>(kind));
  Store32(&record, kRecordOffsetFlags, 0);
  Store64(&record, kRecordOffsetSequence, sequence);
  Store64(&record, kRecordOffsetPayloadBytes, static_cast<u64>(payload.size()));
  Store16(&record, kRecordOffsetPayloadDigestBytes, kProtectedMaterialDigestBytes);
  record.insert(record.end(), payload.begin(), payload.end());
  if (!AttachSha256Digest(&record, kRecordOffsetDigest, kProtectedMaterialDigestBytes)) {
    return false;
  }
  encoded->insert(encoded->end(), record.begin(), record.end());
  return true;
}

bool DecodeProtectedMaterialRecord(const std::vector<byte>& catalog,
                                   std::size_t* offset,
                                   u64* last_sequence,
                                   const std::string& expected_database_uuid,
                                   ProtectedMaterialCatalogImage* image,
                                   std::string* detail) {
  if (*offset > catalog.size() ||
      catalog.size() - *offset < kProtectedMaterialRecordHeaderBytes) {
    *detail = "protected_material_catalog_record_truncated";
    return false;
  }
  const std::size_t record_start = *offset;
  if (!MagicEquals(catalog, record_start, kProtectedMaterialRecordMagic)) {
    *detail = "protected_material_catalog_record_magic_invalid";
    return false;
  }
  const u16 version = Load16(catalog, record_start + kRecordOffsetVersion);
  const u16 header_bytes = Load16(catalog, record_start + kRecordOffsetHeaderBytes);
  const auto kind = static_cast<ProtectedMaterialRecordKind>(
      Load16(catalog, record_start + kRecordOffsetKind));
  const u64 sequence = Load64(catalog, record_start + kRecordOffsetSequence);
  const u64 payload_bytes = Load64(catalog, record_start + kRecordOffsetPayloadBytes);
  const u16 digest_bytes = Load16(catalog, record_start + kRecordOffsetPayloadDigestBytes);
  if (version != kProtectedMaterialCatalogVersion ||
      header_bytes != kProtectedMaterialRecordHeaderBytes ||
      digest_bytes != kProtectedMaterialDigestBytes) {
    *detail = "protected_material_catalog_record_header_invalid";
    return false;
  }
  if (kind != ProtectedMaterialRecordKind::material &&
      kind != ProtectedMaterialRecordKind::version &&
      kind != ProtectedMaterialRecordKind::audit) {
    *detail = "protected_material_catalog_record_kind_invalid";
    return false;
  }
  if (sequence == 0 || sequence <= *last_sequence) {
    *detail = "protected_material_catalog_record_sequence_invalid";
    return false;
  }
  if (payload_bytes > std::numeric_limits<std::size_t>::max() ||
      catalog.size() - record_start - header_bytes < payload_bytes) {
    *detail = "protected_material_catalog_record_payload_truncated";
    return false;
  }
  const std::size_t record_bytes = header_bytes + static_cast<std::size_t>(payload_bytes);
  std::vector<byte> record(catalog.begin() + static_cast<std::ptrdiff_t>(record_start),
                           catalog.begin() + static_cast<std::ptrdiff_t>(record_start + record_bytes));
  if (!Sha256DigestMatches(record, kRecordOffsetDigest, digest_bytes)) {
    *detail = "protected_material_catalog_record_digest_mismatch";
    return false;
  }
  const std::vector<byte> payload(
      catalog.begin() + static_cast<std::ptrdiff_t>(record_start + header_bytes),
      catalog.begin() + static_cast<std::ptrdiff_t>(record_start + record_bytes));
  std::string record_database_uuid;
  if (kind == ProtectedMaterialRecordKind::material) {
    EngineProtectedMaterialCatalogEntry material;
    if (!DeserializeMaterialPayload(payload, &material)) {
      *detail = "protected_material_catalog_material_payload_invalid";
      return false;
    }
    record_database_uuid = material.database_uuid;
    image->materials.push_back(std::move(material));
  } else if (kind == ProtectedMaterialRecordKind::version) {
    EngineProtectedMaterialVersionCatalogEntry material_version;
    if (!DeserializeVersionPayload(payload, &material_version)) {
      *detail = "protected_material_catalog_version_payload_invalid";
      return false;
    }
    record_database_uuid = material_version.database_uuid;
    image->versions.push_back(std::move(material_version));
  } else {
    EngineProtectedMaterialAuditEvent event;
    if (!DeserializeAuditPayload(payload, &event)) {
      *detail = "protected_material_catalog_audit_payload_invalid";
      return false;
    }
    record_database_uuid = event.database_uuid;
    image->audit_events.push_back(std::move(event));
  }
  if (record_database_uuid.empty() ||
      (!expected_database_uuid.empty() && record_database_uuid != expected_database_uuid)) {
    *detail = "protected_material_catalog_database_uuid_mismatch";
    return false;
  }
  *last_sequence = sequence;
  *offset = record_start + record_bytes;
  return true;
}

u64 MaxCatalogGeneration(const ProtectedMaterialCatalogImage& image) {
  u64 generation = image.generation;
  for (const auto& material : image.materials) {
    generation = std::max<u64>(generation, material.catalog_generation_id);
  }
  for (const auto& version : image.versions) {
    generation = std::max<u64>(generation, version.catalog_generation_id);
  }
  for (const auto& event : image.audit_events) {
    generation = std::max<u64>(generation, event.catalog_generation_id);
  }
  return generation;
}

std::vector<byte> EncodeProtectedMaterialCatalog(ProtectedMaterialCatalogImage image) {
  image.generation = MaxCatalogGeneration(image);
  std::vector<byte> record_bytes;
  u64 sequence = 0;
  for (const auto& material : image.materials) {
    std::vector<byte> payload;
    if (!SerializeMaterialPayload(material, &payload) ||
        !EncodeProtectedMaterialRecord(ProtectedMaterialRecordKind::material,
                                       ++sequence,
                                       payload,
                                       &record_bytes)) {
      return {};
    }
  }
  for (const auto& version : image.versions) {
    std::vector<byte> payload;
    if (!SerializeVersionPayload(version, &payload) ||
        !EncodeProtectedMaterialRecord(ProtectedMaterialRecordKind::version,
                                       ++sequence,
                                       payload,
                                       &record_bytes)) {
      return {};
    }
  }
  for (const auto& event : image.audit_events) {
    std::vector<byte> payload;
    if (!SerializeAuditPayload(event, &payload) ||
        !EncodeProtectedMaterialRecord(ProtectedMaterialRecordKind::audit,
                                       ++sequence,
                                       payload,
                                       &record_bytes)) {
      return {};
    }
  }

  std::vector<byte> encoded(kProtectedMaterialCatalogHeaderBytes);
  std::copy(kProtectedMaterialCatalogMagic.begin(),
            kProtectedMaterialCatalogMagic.end(),
            encoded.begin());
  Store16(&encoded, kCatalogOffsetVersion, kProtectedMaterialCatalogVersion);
  Store16(&encoded, kCatalogOffsetHeaderBytes, kProtectedMaterialCatalogHeaderBytes);
  Store32(&encoded, kCatalogOffsetFlags, 0);
  Store64(&encoded, kCatalogOffsetGeneration, image.generation);
  Store64(&encoded, kCatalogOffsetRecordCount, sequence);
  Store64(&encoded, kCatalogOffsetRecordBytes, static_cast<u64>(record_bytes.size()));
  Store16(&encoded, kCatalogOffsetDigestBytes, kProtectedMaterialDigestBytes);
  encoded.insert(encoded.end(), record_bytes.begin(), record_bytes.end());
  if (!AttachSha256Digest(&encoded, kCatalogOffsetDigest, kProtectedMaterialDigestBytes)) {
    return {};
  }
  return encoded;
}

ProtectedMaterialCatalogLoadResult LoadProtectedMaterialCatalogFile(
    const EngineRequestContext& context,
    const std::string& expected_database_uuid) {
  const auto path_status = ValidateProtectedMaterialCatalogPath(context);
  if (path_status.error) { return ProtectedMaterialCatalogError(path_status.detail); }
  const std::string path = ProtectedMaterialCatalogPath(context);
  std::error_code ec;
  const bool present = std::filesystem::exists(path, ec);
  if (ec) {
    return ProtectedMaterialCatalogError("protected_material_catalog_stat_failed:" + ec.message());
  }
  if (!present) { return ProtectedMaterialCatalogAbsent(); }

  std::ifstream in(path, std::ios::binary);
  if (!in) { return ProtectedMaterialCatalogError("protected_material_catalog_open_failed"); }
  std::vector<byte> encoded((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  if (!in.eof() && in.bad()) {
    return ProtectedMaterialCatalogError("protected_material_catalog_read_failed");
  }
  if (encoded.size() < kProtectedMaterialCatalogHeaderBytes ||
      !MagicEquals(encoded, 0, kProtectedMaterialCatalogMagic)) {
    return ProtectedMaterialCatalogError("protected_material_catalog_header_invalid");
  }
  const u16 version = Load16(encoded, kCatalogOffsetVersion);
  const u16 header_bytes = Load16(encoded, kCatalogOffsetHeaderBytes);
  const u64 generation = Load64(encoded, kCatalogOffsetGeneration);
  const u64 record_count = Load64(encoded, kCatalogOffsetRecordCount);
  const u64 record_bytes = Load64(encoded, kCatalogOffsetRecordBytes);
  const u16 digest_bytes = Load16(encoded, kCatalogOffsetDigestBytes);
  if (version != kProtectedMaterialCatalogVersion ||
      header_bytes != kProtectedMaterialCatalogHeaderBytes ||
      digest_bytes != kProtectedMaterialDigestBytes) {
    return ProtectedMaterialCatalogError("protected_material_catalog_header_invalid");
  }
  if (record_bytes > std::numeric_limits<std::size_t>::max() ||
      encoded.size() != static_cast<std::size_t>(header_bytes) +
                            static_cast<std::size_t>(record_bytes)) {
    return ProtectedMaterialCatalogError("protected_material_catalog_size_mismatch");
  }
  if (record_count > 0 &&
      record_count > record_bytes / kProtectedMaterialRecordHeaderBytes) {
    return ProtectedMaterialCatalogError("protected_material_catalog_record_count_invalid");
  }
  if (!Sha256DigestMatches(encoded, kCatalogOffsetDigest, digest_bytes)) {
    return ProtectedMaterialCatalogError("protected_material_catalog_digest_mismatch");
  }

  ProtectedMaterialCatalogLoadResult result;
  result.ok = true;
  result.present = true;
  result.image.generation = generation;
  std::size_t offset = header_bytes;
  u64 last_sequence = 0;
  for (u64 i = 0; i < record_count; ++i) {
    std::string detail;
    if (!DecodeProtectedMaterialRecord(encoded,
                                       &offset,
                                       &last_sequence,
                                       expected_database_uuid,
                                       &result.image,
                                       &detail)) {
      return ProtectedMaterialCatalogError(detail);
    }
  }
  if (offset != encoded.size()) {
    return ProtectedMaterialCatalogError("protected_material_catalog_trailing_bytes");
  }
  if ((record_count == 0 && generation != 0) ||
      (record_count > 0 && generation == 0) ||
      MaxCatalogGeneration(result.image) > generation) {
    return ProtectedMaterialCatalogError("protected_material_catalog_generation_invalid");
  }
  return result;
}

bool ReplaceProtectedMaterialCatalogAtomically(const std::filesystem::path& temp_path,
                                               const std::filesystem::path& target_path,
                                               std::string* detail) {
#if defined(_WIN32)
  if (::MoveFileExW(temp_path.wstring().c_str(),
                    target_path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return true;
  }
  if (detail != nullptr) { *detail = "win32_error=" + std::to_string(::GetLastError()); }
  return false;
#else
  std::error_code ec;
  std::filesystem::rename(temp_path, target_path, ec);
  if (!ec) { return true; }
  if (detail != nullptr) { *detail = ec.message(); }
  return false;
#endif
}

EngineApiDiagnostic PersistProtectedMaterialCatalogFile(
    const EngineRequestContext& context,
    const ProtectedMaterialCatalogImage& image) {
  const auto path_status = ValidateProtectedMaterialCatalogPath(context);
  if (path_status.error) { return path_status; }
  const std::vector<byte> encoded = EncodeProtectedMaterialCatalog(image);
  if (encoded.empty()) {
    return ProtectedMaterialCatalogDiagnostic("protected_material_catalog_encode_failed");
  }
  const std::filesystem::path target_path = ProtectedMaterialCatalogPath(context);
  const std::filesystem::path temp_path = target_path.string() + ".tmp";
  std::error_code ec;
  const bool temp_present = std::filesystem::exists(temp_path, ec);
  if (ec) {
    return ProtectedMaterialCatalogDiagnostic(
        "protected_material_catalog_stale_temp_stat_failed:" + ec.message());
  }
  if (temp_present) {
    std::filesystem::remove(temp_path, ec);
    if (ec) {
      return ProtectedMaterialCatalogDiagnostic(
          "protected_material_catalog_stale_temp_remove_failed:" + ec.message());
    }
    const auto parent_sync = disk::SyncParentDirectoryPath(temp_path.string());
    if (!parent_sync.ok()) {
      return ProtectedMaterialCatalogDiagnostic(
          "protected_material_catalog_parent_sync_failed:" +
          parent_sync.diagnostic.diagnostic_code);
    }
  }

  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return ProtectedMaterialCatalogDiagnostic("protected_material_catalog_temp_open_failed");
    }
    out.write(reinterpret_cast<const char*>(encoded.data()),
              static_cast<std::streamsize>(encoded.size()));
    out.close();
    if (!out) {
      return ProtectedMaterialCatalogDiagnostic("protected_material_catalog_temp_write_failed");
    }
  }

  const auto file_sync = disk::SyncFilesystemPath(temp_path.string(), true);
  if (!file_sync.ok()) {
    return ProtectedMaterialCatalogDiagnostic(
        "protected_material_catalog_file_sync_failed:" +
        file_sync.diagnostic.diagnostic_code);
  }
  std::string replace_detail;
  if (!ReplaceProtectedMaterialCatalogAtomically(temp_path, target_path, &replace_detail)) {
    return ProtectedMaterialCatalogDiagnostic(
        "protected_material_catalog_rename_failed:" + replace_detail);
  }
  const auto parent_sync = disk::SyncParentDirectoryPath(target_path.string());
  if (!parent_sync.ok()) {
    return ProtectedMaterialCatalogDiagnostic(
        "protected_material_catalog_parent_sync_failed:" +
        parent_sync.diagnostic.diagnostic_code);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

ProtectedMaterialCatalogImage CaptureProtectedMaterialCatalogImageLocked(
    const std::string& database_uuid) {
  ProtectedMaterialCatalogImage image;
  image.generation = MaterialCatalogGenerationCounter();
  for (const auto& material : MaterialCatalog()) {
    if (material.database_uuid == database_uuid) { image.materials.push_back(material); }
  }
  for (const auto& version : MaterialVersions()) {
    if (version.database_uuid == database_uuid) { image.versions.push_back(version); }
  }
  for (const auto& event : MaterialAuditEvents()) {
    if (event.database_uuid == database_uuid) { image.audit_events.push_back(event); }
  }
  return image;
}

template <typename Entry>
void EraseDatabaseEntries(std::vector<Entry>* entries, const std::string& database_uuid) {
  entries->erase(std::remove_if(entries->begin(),
                                entries->end(),
                                [&](const Entry& entry) {
                                  return entry.database_uuid == database_uuid;
                                }),
                 entries->end());
}

void RefreshProtectedMaterialGenerationCounterLocked() {
  u64 generation = 0;
  for (const auto& material : MaterialCatalog()) {
    generation = std::max<u64>(generation, material.catalog_generation_id);
  }
  for (const auto& version : MaterialVersions()) {
    generation = std::max<u64>(generation, version.catalog_generation_id);
  }
  for (const auto& event : MaterialAuditEvents()) {
    generation = std::max<u64>(generation, event.catalog_generation_id);
  }
  MaterialCatalogGenerationCounter() =
      std::max<std::uint64_t>(MaterialCatalogGenerationCounter(), generation);
}

void ReplaceProtectedMaterialCatalogImageLocked(
    const std::string& database_uuid,
    const ProtectedMaterialCatalogImage& image) {
  EraseDatabaseEntries(&MaterialCatalog(), database_uuid);
  EraseDatabaseEntries(&MaterialVersions(), database_uuid);
  EraseDatabaseEntries(&MaterialAuditEvents(), database_uuid);
  MaterialCatalog().insert(MaterialCatalog().end(), image.materials.begin(), image.materials.end());
  MaterialVersions().insert(MaterialVersions().end(), image.versions.begin(), image.versions.end());
  MaterialAuditEvents().insert(MaterialAuditEvents().end(),
                               image.audit_events.begin(),
                               image.audit_events.end());
  MaterialCatalogGenerationCounter() =
      std::max<std::uint64_t>(MaterialCatalogGenerationCounter(), image.generation);
  RefreshProtectedMaterialGenerationCounterLocked();
}

EngineApiDiagnostic LoadProtectedMaterialCatalogForDatabaseLocked(
    const EngineRequestContext& context,
    const std::string& database_uuid) {
  const auto loaded = LoadProtectedMaterialCatalogFile(context, database_uuid);
  if (!loaded.ok) { return loaded.diagnostic; }
  ProtectedMaterialCatalogImage image = loaded.present
      ? loaded.image
      : ProtectedMaterialCatalogImage{};
  ReplaceProtectedMaterialCatalogImageLocked(database_uuid, image);
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic PersistProtectedMaterialCatalogForDatabaseLocked(
    const EngineRequestContext& context,
    const std::string& database_uuid) {
  auto image = CaptureProtectedMaterialCatalogImageLocked(database_uuid);
  image.generation = MaterialCatalogGenerationCounter();
  return PersistProtectedMaterialCatalogFile(context, image);
}

EngineApiDiagnostic PersistProtectedMaterialCatalogMutationLocked(
    const EngineRequestContext& context,
    const std::string& database_uuid,
    const ProtectedMaterialCatalogImage& before_mutation) {
  const auto persisted = PersistProtectedMaterialCatalogForDatabaseLocked(context, database_uuid);
  if (persisted.error) {
    ReplaceProtectedMaterialCatalogImageLocked(database_uuid, before_mutation);
    return persisted;
  }
  return persisted;
}

std::uint64_t RequestTime(const EngineRequestContext& context) {
  return context.resource_epoch == 0 ? 1 : context.resource_epoch;
}

std::string DatabaseUuidFromRequest(const EngineApiRequest& request) {
  if (!request.context.database_uuid.canonical.empty()) {
    return request.context.database_uuid.canonical;
  }
  return request.target_database.uuid.canonical;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  return SecurityOptionValue(request, prefix);
}

bool OptionEnabled(const EngineApiRequest& request, const std::string& prefix) {
  return SecurityOptionBool(request, prefix, false);
}

std::string FingerprintFor(const std::string& database_uuid,
                           const std::string& key_uuid,
                           const std::string& filespace_uuid,
                           const std::string& evidence) {
  const std::string framed = "scratchbird.protected_material.fingerprint.v1|" +
                             database_uuid + "|" + key_uuid + "|" + filespace_uuid;
  return "fingerprint:v1:hmac-sha256:" +
         SecurityHmacSha256Hex(evidence, framed);
}

std::string HandleFor(const std::string& database_uuid,
                      const std::string& filespace_uuid,
                      const std::string& key_uuid,
                      std::uint64_t generation,
                      const std::string& key_fingerprint) {
  const std::string framed = "scratchbird.protected_material.handle.v1|" + database_uuid + "|" +
                             filespace_uuid + "|" + key_uuid + "|" +
                             std::to_string(generation) + "|" + key_fingerprint;
  return "protected-material-handle:v1:hmac-sha256:" +
         SecurityHmacSha256Hex(key_fingerprint, framed) + ":" +
         std::to_string(generation);
}

bool ContainsProtectedMaterialMarker(const std::string& text) {
  const std::string lower = SecurityLower(text);
  const std::vector<std::string> markers = {
      "secret", "password", "passwd", "pwd=", "credential", "verifier",
      "private_key", "key_material", "plaintext", "cleartext", "encryption_key",
      "decryption_key", "protected_material", "bearer ", "token=", "apikey",
      "api_key", "kms_plaintext"};
  for (const auto& marker : markers) {
    if (lower.find(marker) != std::string::npos) { return true; }
  }
  return false;
}

bool PlaintextEvidenceRefused(const std::string& evidence) {
  if (evidence.empty()) { return true; }
  const std::string lower = SecurityLower(evidence);
  const std::vector<std::string> refused = {
      "plaintext:", "cleartext:", "password:", "password=", "passwd=",
      "secret=", "private_key=", "key_material=", "raw_key=", "kms_plaintext:"};
  for (const auto& marker : refused) {
    if (lower.find(marker) != std::string::npos) { return true; }
  }
  return false;
}

EngineApiDiagnostic AuthorityBypassDiagnostic(const std::string& operation_id,
                                              const std::string& detail) {
  return MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.AUTHORITY_BYPASS_REFUSED",
                                operation_id + ":" + detail);
}

EngineApiDiagnostic ValidateEngineAuthorityBoundary(const EngineApiRequest& request,
                                                    const std::string& operation_id) {
  const std::vector<std::string> authority_prefixes = {
      "auth_authority:", "key_authority:", "protected_material_authority:",
      "filespace_open_authority:", "encryption_authority:", "storage_authority:"};
  for (const auto& prefix : authority_prefixes) {
    const auto value = SecurityLower(OptionValue(request, prefix));
    if (!value.empty() && value != "engine" && value != "engine_internal") {
      return AuthorityBypassDiagnostic(operation_id, prefix + "not_engine");
    }
  }
  for (const auto& tag : request.context.trace_tags) {
    const auto lower = SecurityLower(tag);
    if (StartsWith(lower, "authority:parser") || StartsWith(lower, "authority:driver") ||
        StartsWith(lower, "authority:donor") || StartsWith(lower, "authority:sqlite")) {
      return AuthorityBypassDiagnostic(operation_id, "non_engine_trace_authority");
    }
  }
  if (OptionEnabled(request, "donor_shortcut:") ||
      OptionEnabled(request, "sqlite_shortcut:") ||
      OptionEnabled(request, "authoritative_wal:")) {
    return AuthorityBypassDiagnostic(operation_id, "donor_sqlite_wal_shortcut_forbidden");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic RequireSpecificRight(const EngineApiRequest& request,
                                         const std::string& operation_id,
                                         const std::string& right) {
  const auto boundary = ValidateEngineAuthorityBoundary(request, operation_id);
  if (boundary.error) { return boundary; }
  if (!SecurityContextHasRight(request.context, right)) {
    return MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.AUTHORITY_DENIED",
                                  operation_id + ":" + right);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic RequireShutdownAuthority(const EngineApiRequest& request,
                                             const std::string& operation_id) {
  const auto boundary = ValidateEngineAuthorityBoundary(request, operation_id);
  if (boundary.error) { return boundary; }
  if (SecurityContextHasRight(request.context, "KEY_RELEASE_APPROVE") ||
      OptionValue(request, "shutdown_authority:") == "engine" ||
      SecurityContextHasTag(request.context, "engine.shutdown")) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }
  return MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.AUTHORITY_DENIED",
                                operation_id + ":shutdown_authority_required");
}

void AddProtectedMaterialRow(EngineApiResult* result,
                             std::vector<std::pair<std::string, std::string>> fields) {
  for (auto& field : fields) { field.second = RedactProtectedMaterialForDiagnostics(std::move(field.second)); }
  AddSecurityRow(result, std::move(fields));
}

void ExpireActiveEntriesLocked(std::uint64_t now) {
  for (auto& entry : Cache()) {
    if (entry.active && !entry.purged && entry.expires_at_epoch_millis <= now) {
      entry.active = false;
      entry.expired = true;
    }
  }
}

EngineProtectedMaterialCacheEntry* FindEntryLocked(const std::string& database_uuid,
                                                   const std::string& filespace_uuid,
                                                   const std::string& key_uuid,
                                                   const std::string& key_handle,
                                                   bool active_only) {
  for (auto& entry : Cache()) {
    if (active_only && (!entry.active || entry.purged || entry.expired)) { continue; }
    if (!database_uuid.empty() && entry.database_uuid != database_uuid) { continue; }
    if (!filespace_uuid.empty() && entry.filespace_uuid != filespace_uuid) { continue; }
    if (!key_uuid.empty() && entry.key_uuid != key_uuid) { continue; }
    if (!key_handle.empty() && entry.key_handle != key_handle) { continue; }
    return &entry;
  }
  return nullptr;
}

EngineProtectedMaterialCacheEntry* FindAnyHandleLocked(const std::string& key_handle) {
  if (key_handle.empty()) { return nullptr; }
  for (auto& entry : Cache()) {
    if (entry.key_handle == key_handle) { return &entry; }
  }
  return nullptr;
}

bool HasExpiredCandidateLocked(const std::string& database_uuid,
                               const std::string& filespace_uuid,
                               const std::string& key_uuid,
                               const std::string& key_handle) {
  for (const auto& entry : Cache()) {
    if (!database_uuid.empty() && entry.database_uuid != database_uuid) { continue; }
    if (!filespace_uuid.empty() && entry.filespace_uuid != filespace_uuid) { continue; }
    if (!key_uuid.empty() && entry.key_uuid != key_uuid) { continue; }
    if (!key_handle.empty() && entry.key_handle != key_handle) { continue; }
    if (entry.expired && !entry.purged) { return true; }
  }
  return false;
}

std::uint64_t PurgeLocked() {
  std::uint64_t count = 0;
  for (auto& entry : Cache()) {
    if (entry.active && !entry.purged) { ++count; }
    entry.active = false;
    entry.purged = true;
    entry.expired = false;
  }
  return count;
}

EngineProtectedMaterialCacheEntry AdmitLocked(const EngineAdmitEncryptionKeyRequest& request,
                                              const std::string& database_uuid,
                                              std::uint64_t admitted_at) {
  for (auto& entry : Cache()) {
    if (entry.database_uuid == database_uuid &&
        entry.key_uuid == request.key_uuid &&
        entry.filespace_uuid == request.filespace_uuid &&
        entry.active) {
      entry.active = false;
      entry.purged = true;
    }
  }

  const std::uint64_t generation = ++GenerationCounter();
  EngineProtectedMaterialCacheEntry entry;
  entry.database_uuid = database_uuid;
  entry.key_uuid = request.key_uuid;
  entry.key_label = request.key_label.empty() ? "redacted-key" : request.key_label;
  entry.filespace_uuid = request.filespace_uuid;
  entry.generation = generation;
  entry.admitted_at_epoch_millis = admitted_at;
  entry.expires_at_epoch_millis = admitted_at + std::max<std::uint64_t>(1, request.cache_ttl_millis);
  entry.key_fingerprint = FingerprintFor(entry.database_uuid,
                                         entry.key_uuid,
                                         entry.filespace_uuid,
                                         request.secret_evidence);
  entry.key_handle = HandleFor(entry.database_uuid,
                               entry.filespace_uuid,
                               entry.key_uuid,
                               entry.generation,
                               entry.key_fingerprint);
  entry.active = true;
  Cache().push_back(entry);
  return entry;
}

bool IsUuidText(const std::string& value) {
  if (value.size() != 36) { return false; }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') { return false; }
      continue;
    }
    if (!std::isxdigit(static_cast<unsigned char>(value[index]))) { return false; }
  }
  return true;
}

bool IsKnownStorageClass(const std::string& storage_class) {
  return storage_class == "direct" || storage_class == "wrapped" ||
         storage_class == "split" || storage_class == "external_reference" ||
         storage_class == "derived" || storage_class == "redacted";
}

bool IsKnownPurposeClass(const std::string& purpose_class) {
  return purpose_class == "security_use" || purpose_class == "encryption_use" ||
         purpose_class == "audit_use" || purpose_class == "donor_use" ||
         purpose_class == "UDR_use" || purpose_class == "udr_use" ||
         purpose_class == "application_use" ||
         purpose_class == "compliance_use" || purpose_class == "cloud_ops_use";
}

bool ContainsPlaintextSecretMarker(const std::string& value) {
  if (value.empty()) { return false; }
  const std::string lower = SecurityLower(value);
  const std::vector<std::string> refused = {
      "plaintext:", "cleartext:", "password:", "password=", "passwd=",
      "secret=", "private_key=", "key_material=", "raw_key=", "token=",
      "bearer ", "kms_plaintext:"};
  for (const auto& marker : refused) {
    if (lower.find(marker) != std::string::npos) { return true; }
  }
  return false;
}

bool ProtectedPayloadInputRefused(const std::string& protected_reference,
                                  const std::string& envelope_reference,
                                  const std::string& payload_hash) {
  return ContainsPlaintextSecretMarker(protected_reference) ||
         ContainsPlaintextSecretMarker(envelope_reference) ||
         ContainsPlaintextSecretMarker(payload_hash);
}

bool ProtectedPayloadPresent(const std::string& protected_reference,
                             const std::string& envelope_reference,
                             const std::string& payload_hash) {
  return !protected_reference.empty() || !envelope_reference.empty() || !payload_hash.empty();
}

bool PolicySetComplete(const EngineProtectedMaterialPolicySet& policy) {
  return IsUuidText(policy.retention_policy_uuid) &&
         IsUuidText(policy.access_policy_uuid) &&
         IsUuidText(policy.release_policy_uuid) &&
         IsUuidText(policy.purge_policy_uuid) &&
         IsUuidText(policy.audit_policy_uuid);
}

EngineProtectedMaterialPolicySet MergePolicy(EngineProtectedMaterialPolicySet base,
                                             const EngineProtectedMaterialPolicySet& overlay) {
  if (!overlay.retention_policy_uuid.empty()) { base.retention_policy_uuid = overlay.retention_policy_uuid; }
  if (!overlay.access_policy_uuid.empty()) { base.access_policy_uuid = overlay.access_policy_uuid; }
  if (!overlay.release_policy_uuid.empty()) { base.release_policy_uuid = overlay.release_policy_uuid; }
  if (!overlay.purge_policy_uuid.empty()) { base.purge_policy_uuid = overlay.purge_policy_uuid; }
  if (!overlay.audit_policy_uuid.empty()) { base.audit_policy_uuid = overlay.audit_policy_uuid; }
  if (overlay.retention_until_epoch_millis != 0) {
    base.retention_until_epoch_millis = overlay.retention_until_epoch_millis;
  }
  base.legal_hold = base.legal_hold || overlay.legal_hold;
  if (!overlay.release_purposes.empty()) { base.release_purposes = overlay.release_purposes; }
  return base;
}

std::uint64_t MutationTransactionId(const EngineRequestContext& context) {
  return context.local_transaction_id;
}

std::uint64_t ReadVisibilityPoint(const EngineRequestContext& context) {
  std::uint64_t point = context.snapshot_visible_through_local_transaction_id;
  if (context.local_transaction_id != 0 && context.local_transaction_id > point) {
    point = context.local_transaction_id;
  }
  return point == 0 ? UINT64_MAX : point;
}

EngineApiDiagnostic ValidateMutationContext(const EngineApiRequest& request,
                                            const std::string& operation_id) {
  if (DatabaseUuidFromRequest(request).empty()) {
    return MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID",
                                  operation_id + ":database_uuid_required");
  }
  if (MutationTransactionId(request.context) == 0) {
    return MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.MGA_CONTEXT_REQUIRED",
                                  operation_id + ":local_transaction_id_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineProtectedMaterialCatalogEntry* FindMaterialLocked(const std::string& database_uuid,
                                                        const std::string& protected_material_uuid) {
  for (auto& material : MaterialCatalog()) {
    if (material.database_uuid == database_uuid &&
        material.protected_material_uuid == protected_material_uuid) {
      return &material;
    }
  }
  return nullptr;
}

EngineProtectedMaterialVersionCatalogEntry* FindVersionLocked(
    const std::string& database_uuid,
    const std::string& protected_material_uuid,
    const std::string& protected_material_version_uuid) {
  for (auto& version : MaterialVersions()) {
    if (version.database_uuid == database_uuid &&
        version.protected_material_uuid == protected_material_uuid &&
        version.protected_material_version_uuid == protected_material_version_uuid) {
      return &version;
    }
  }
  return nullptr;
}

std::uint64_t NextVersionNumberLocked(const std::string& database_uuid,
                                      const std::string& protected_material_uuid) {
  std::uint64_t max_version = 0;
  for (const auto& version : MaterialVersions()) {
    if (version.database_uuid == database_uuid &&
        version.protected_material_uuid == protected_material_uuid &&
        version.version_number > max_version) {
      max_version = version.version_number;
    }
  }
  return max_version + 1;
}

bool VersionVisibleAt(const EngineProtectedMaterialVersionCatalogEntry& version,
                      std::uint64_t visibility_point) {
  if (version.purged) { return false; }
  if (version.valid_from_local_transaction_id > visibility_point) { return false; }
  return version.valid_until_local_transaction_id == 0 ||
         version.valid_until_local_transaction_id > visibility_point;
}

EngineProtectedMaterialVersionCatalogEntry* ResolveActiveVersionLocked(
    const EngineRequestContext& context,
    const std::string& database_uuid,
    const std::string& protected_material_uuid) {
  const std::uint64_t visibility_point = ReadVisibilityPoint(context);
  EngineProtectedMaterialVersionCatalogEntry* selected = nullptr;
  for (auto& version : MaterialVersions()) {
    if (version.database_uuid != database_uuid ||
        version.protected_material_uuid != protected_material_uuid) {
      continue;
    }
    if (!VersionVisibleAt(version, visibility_point)) { continue; }
    if (selected == nullptr || version.version_number > selected->version_number) {
      selected = &version;
    }
  }
  return selected;
}

bool PurposeAllowed(const EngineProtectedMaterialVersionCatalogEntry& version,
                    const EngineProtectedMaterialCatalogEntry& material,
                    const std::string& purpose) {
  const auto& allowlist = version.policy.release_purposes.empty()
      ? material.policy.release_purposes
      : version.policy.release_purposes;
  if (allowlist.empty()) { return purpose == material.purpose_class; }
  return std::find(allowlist.begin(), allowlist.end(), purpose) != allowlist.end();
}

EngineProtectedMaterialVersionCatalogEntry RedactedVersion(
    EngineProtectedMaterialVersionCatalogEntry version) {
  if (!version.protected_reference.empty()) {
    version.protected_reference = "<protected-material-redacted>";
  }
  if (!version.envelope_reference.empty()) {
    version.envelope_reference = "<protected-material-redacted>";
  }
  return version;
}

std::string ProtectedMaterialRefFor(const EngineProtectedMaterialVersionCatalogEntry& version) {
  const std::string framed = "scratchbird.protected_material.ref.v1|" +
                             version.database_uuid + "|" +
                             version.protected_material_uuid + "|" +
                             version.protected_material_version_uuid + "|" +
                             std::to_string(version.version_number) + "|" +
                             version.payload_hash;
  return "protected-material-ref:v1:sha256:" + SecuritySha256Hex(framed) + ":" +
         std::to_string(version.version_number);
}

std::string ReleaseHandleFor(const EngineProtectedMaterialVersionCatalogEntry& version,
                             const std::string& purpose,
                             std::uint64_t generation) {
  const std::string framed = "scratchbird.protected_material.release.v1|" +
                             version.database_uuid + "|" +
                             version.protected_material_uuid + "|" +
                             version.protected_material_version_uuid + "|" +
                             purpose + "|" + std::to_string(generation) + "|" +
                             version.payload_hash + "|" + version.protected_reference;
  const std::string key = version.protected_reference.empty()
      ? version.payload_hash
      : version.protected_reference;
  return "protected-material-release:v1:hmac-sha256:" +
         SecurityHmacSha256Hex(key, framed) + ":" +
         std::to_string(generation);
}

EngineProtectedMaterialAuditEvent AppendMaterialAuditLocked(
    const EngineRequestContext& context,
    const std::string& database_uuid,
    const std::string& protected_material_uuid,
    const std::string& protected_material_version_uuid,
    const std::string& event_kind,
    const std::string& decision,
    const std::string& diagnostic_code,
    const std::string& detail,
    std::uint64_t catalog_generation_id) {
  EngineProtectedMaterialAuditEvent event;
  event.database_uuid = database_uuid;
  event.protected_material_uuid = protected_material_uuid;
  event.protected_material_version_uuid = protected_material_version_uuid;
  event.actor_uuid = context.principal_uuid.canonical;
  event.event_kind = event_kind;
  event.decision = decision;
  event.diagnostic_code = diagnostic_code;
  event.redacted_detail = RedactProtectedMaterialForDiagnostics(detail);
  event.event_epoch_millis = RequestTime(context);
  event.local_transaction_id = context.local_transaction_id;
  event.catalog_generation_id = catalog_generation_id;
  event.redaction_applied = true;
  const std::string framed = database_uuid + "|" + protected_material_uuid + "|" +
                             protected_material_version_uuid + "|" + event_kind + "|" +
                             decision + "|" + std::to_string(event.event_epoch_millis) + "|" +
                             std::to_string(MaterialAuditEvents().size() + 1);
  event.audit_event_uuid = "protected-material-audit:v1:sha256:" +
                           SecuritySha256Hex(framed);
  MaterialAuditEvents().push_back(event);
  return event;
}

void AddMaterialAuditEvidence(EngineApiResult* result,
                              const EngineProtectedMaterialAuditEvent& event) {
  AddSecurityEvidence(result, "protected_material_audit_event", event.audit_event_uuid);
}

void AddMaterialCatalogResultRow(EngineApiResult* result,
                                 const EngineProtectedMaterialCatalogEntry& material,
                                 const std::string& action) {
  AddProtectedMaterialRow(result,
                          {{"row_class", "protected_material"},
                           {"action", action},
                           {"database_uuid", material.database_uuid},
                           {"protected_material_uuid", material.protected_material_uuid},
                           {"active_version_uuid", material.active_version_uuid},
                           {"purpose_class", material.purpose_class},
                           {"storage_class", material.storage_class},
                           {"lifecycle_state", material.lifecycle_state},
                           {"catalog_generation_id", std::to_string(material.catalog_generation_id)},
                           {"created_local_transaction_id", std::to_string(material.created_local_transaction_id)},
                           {"updated_local_transaction_id", std::to_string(material.updated_local_transaction_id)},
                           {"protected_material", "<protected-material-redacted>"}});
}

void AddVersionCatalogResultRow(EngineApiResult* result,
                                const EngineProtectedMaterialVersionCatalogEntry& version,
                                const std::string& action) {
  AddProtectedMaterialRow(result,
                          {{"row_class", "protected_material_version"},
                           {"action", action},
                           {"database_uuid", version.database_uuid},
                           {"protected_material_uuid", version.protected_material_uuid},
                           {"protected_material_version_uuid", version.protected_material_version_uuid},
                           {"version_number", std::to_string(version.version_number)},
                           {"storage_class", version.storage_class},
                           {"rotation_state", version.rotation_state},
                           {"active", version.active ? "true" : "false"},
                           {"purged", version.purged ? "true" : "false"},
                           {"retention_until_epoch_millis", std::to_string(version.policy.retention_until_epoch_millis)},
                           {"legal_hold", version.policy.legal_hold ? "true" : "false"},
                           {"protected_reference", version.protected_reference.empty() ? "absent" : "<protected-material-redacted>"},
                           {"envelope_reference", version.envelope_reference.empty() ? "absent" : "<protected-material-redacted>"},
                           {"payload_hash", version.payload_hash},
                           {"protected_material", "<protected-material-redacted>"}});
}

void AddAuditCatalogResultRow(EngineApiResult* result,
                              const EngineProtectedMaterialAuditEvent& event) {
  AddProtectedMaterialRow(result,
                          {{"row_class", "protected_material_audit_event"},
                           {"audit_event_uuid", event.audit_event_uuid},
                           {"database_uuid", event.database_uuid},
                           {"protected_material_uuid", event.protected_material_uuid},
                           {"protected_material_version_uuid", event.protected_material_version_uuid},
                           {"event_kind", event.event_kind},
                           {"decision", event.decision},
                           {"diagnostic_code", event.diagnostic_code},
                           {"detail", event.redacted_detail},
                           {"redaction_applied", event.redaction_applied ? "true" : "false"}});
}

}  // namespace

std::string RedactProtectedMaterialForDiagnostics(std::string text) {
  if (text.empty()) { return text; }
  if (ContainsProtectedMaterialMarker(text)) { return "<protected-material-redacted>"; }
  return text;
}

EngineAdmitEncryptionKeyResult EngineAdmitEncryptionKey(
    const EngineAdmitEncryptionKeyRequest& request) {
  const std::string operation_id = "security.encryption_key.admit";
  auto status = RequireSpecificRight(request, operation_id, "KEY_RELEASE_APPROVE");
  if (status.error) {
    return SecurityFailure<EngineAdmitEncryptionKeyResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (database_uuid.empty() || request.key_uuid.empty() ||
      request.filespace_uuid.empty() || request.secret_evidence.empty()) {
    return SecurityFailure<EngineAdmitEncryptionKeyResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.KEY.ADMISSION_INVALID",
                               "database_uuid_key_uuid_filespace_uuid_secret_evidence_required"));
  }
  if (PlaintextEvidenceRefused(request.secret_evidence)) {
    return SecurityFailure<EngineAdmitEncryptionKeyResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.KEY.PLAINTEXT_REFUSED",
                               "plaintext_secret_material_is_never_admitted"));
  }

  const std::uint64_t admitted_at = RequestTime(request.context);
  EngineProtectedMaterialCacheEntry entry;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    ExpireActiveEntriesLocked(admitted_at);
    entry = AdmitLocked(request, database_uuid, admitted_at);
  }

  auto result = SecuritySuccess<EngineAdmitEncryptionKeyResult>(request.context, operation_id);
  result.key_admitted = true;
  result.cache_entry_active = true;
  result.plaintext_material_returned = false;
  result.key_handle = entry.key_handle;
  result.key_fingerprint = entry.key_fingerprint;
  result.key_generation = entry.generation;
  result.expires_at_epoch_millis = entry.expires_at_epoch_millis;
  AddSecurityEvidence(&result, "protected_material_key_admission", entry.key_uuid);
  AddProtectedMaterialRow(&result,
                          {{"database_uuid", entry.database_uuid},
                           {"key_uuid", entry.key_uuid},
                           {"filespace_uuid", entry.filespace_uuid},
                           {"key_handle", entry.key_handle},
                           {"key_fingerprint", entry.key_fingerprint},
                           {"generation", std::to_string(entry.generation)},
                           {"plaintext_material_returned", "false"},
                           {"secret_evidence", "<protected-material-redacted>"}});
  return result;
}

EngineRotateEncryptionKeyResult EngineRotateEncryptionKey(
    const EngineRotateEncryptionKeyRequest& request) {
  const std::string operation_id = "security.encryption_key.rotate";
  auto status = RequireSpecificRight(request, operation_id, "KEY_RELEASE_APPROVE");
  if (status.error) {
    return SecurityFailure<EngineRotateEncryptionKeyResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (database_uuid.empty() || request.key_uuid.empty() ||
      request.replacement_key_uuid.empty() || request.replacement_secret_evidence.empty()) {
    return SecurityFailure<EngineRotateEncryptionKeyResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.KEY.ROTATION_INVALID",
                               "active_key_and_replacement_required"));
  }
  if (PlaintextEvidenceRefused(request.replacement_secret_evidence)) {
    return SecurityFailure<EngineRotateEncryptionKeyResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.KEY.PLAINTEXT_REFUSED",
                               "plaintext_secret_material_is_never_persisted"));
  }

  EngineProtectedMaterialCacheEntry replacement;
  std::string filespace_uuid;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    ExpireActiveEntriesLocked(RequestTime(request.context));
    auto* previous = FindEntryLocked(database_uuid, {}, request.key_uuid, {}, true);
    if (previous == nullptr) {
      return SecurityFailure<EngineRotateEncryptionKeyResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.KEY.UNAVAILABLE",
                                 "active_key_required_for_rotation"));
    }
    filespace_uuid = previous->filespace_uuid;

    EngineAdmitEncryptionKeyRequest admit;
    admit.context = request.context;
    admit.target_database = request.target_database;
    admit.key_uuid = request.replacement_key_uuid;
    admit.filespace_uuid = filespace_uuid;
    admit.secret_evidence = request.replacement_secret_evidence;
    admit.key_label = "rotated-key";
    admit.cache_ttl_millis = request.cache_ttl_millis;

    previous->active = false;
    previous->purged = true;
    replacement = AdmitLocked(admit, database_uuid, RequestTime(request.context));
  }

  auto result = SecuritySuccess<EngineRotateEncryptionKeyResult>(request.context, operation_id);
  result.rotated = true;
  result.rotation_metadata_persisted = true;
  result.plaintext_material_persisted = false;
  result.previous_key_uuid = request.key_uuid;
  result.active_key_uuid = replacement.key_uuid;
  result.active_key_handle = replacement.key_handle;
  result.active_generation = replacement.generation;
  AddSecurityEvidence(&result, "protected_material_key_rotation", request.key_uuid);
  AddProtectedMaterialRow(&result,
                          {{"database_uuid", database_uuid},
                           {"filespace_uuid", filespace_uuid},
                           {"previous_key_uuid", request.key_uuid},
                           {"active_key_uuid", replacement.key_uuid},
                           {"rotation_reason", request.rotation_reason},
                           {"plaintext_material_persisted", "false"}});
  return result;
}

EngineInspectProtectedMaterialCacheResult EngineInspectProtectedMaterialCache(
    const EngineInspectProtectedMaterialCacheRequest& request) {
  const std::string operation_id = "security.protected_material_cache.inspect";
  auto status = RequireSpecificRight(request, operation_id, "PROTECTED_MATERIAL_RELEASE");
  if (status.error) {
    return SecurityFailure<EngineInspectProtectedMaterialCacheResult>(request.context, operation_id, status);
  }
  auto result = SecuritySuccess<EngineInspectProtectedMaterialCacheResult>(request.context, operation_id);
  result.protected_material_redacted = true;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    ExpireActiveEntriesLocked(RequestTime(request.context));
    for (const auto& entry : Cache()) {
      if (!request.key_uuid.empty() && entry.key_uuid != request.key_uuid) { continue; }
      EngineProtectedMaterialCacheEntry redacted = entry;
      redacted.key_label = RedactProtectedMaterialForDiagnostics(redacted.key_label);
      result.entries.push_back(redacted);
      if (entry.active && !entry.purged && !entry.expired) { ++result.active_entry_count; }
      AddProtectedMaterialRow(&result,
                              {{"database_uuid", entry.database_uuid},
                               {"key_uuid", entry.key_uuid},
                               {"filespace_uuid", entry.filespace_uuid},
                               {"key_handle", entry.key_handle},
                               {"generation", std::to_string(entry.generation)},
                               {"active", entry.active ? "true" : "false"},
                               {"purged", entry.purged ? "true" : "false"},
                               {"expired", entry.expired ? "true" : "false"},
                               {"protected_material", "<protected-material-redacted>"}});
    }
  }
  AddSecurityEvidence(&result, "protected_material_cache_inspect", std::to_string(result.active_entry_count));
  return result;
}

EnginePurgeProtectedMaterialResult EnginePurgeProtectedMaterial(
    const EnginePurgeProtectedMaterialRequest& request) {
  const std::string operation_id = "security.protected_material_cache.purge";
  auto status = RequireSpecificRight(request, operation_id, "KEY_RELEASE_APPROVE");
  if (status.error) {
    return SecurityFailure<EnginePurgeProtectedMaterialResult>(request.context, operation_id, status);
  }
  std::uint64_t count = 0;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    ExpireActiveEntriesLocked(RequestTime(request.context));
    count = PurgeLocked();
  }
  auto result = SecuritySuccess<EnginePurgeProtectedMaterialResult>(request.context, operation_id);
  result.purged = true;
  result.purged_entry_count = count;
  AddSecurityEvidence(&result, "protected_material_cache_purge", std::to_string(count));
  AddProtectedMaterialRow(&result,
                          {{"purged_entry_count", std::to_string(count)},
                           {"purge_reason", request.purge_reason}});
  return result;
}

EngineShutdownProtectedMaterialResult EngineShutdownProtectedMaterial(
    const EngineShutdownProtectedMaterialRequest& request) {
  const std::string operation_id = "security.protected_material_cache.shutdown";
  auto status = RequireShutdownAuthority(request, operation_id);
  if (status.error) {
    return SecurityFailure<EngineShutdownProtectedMaterialResult>(request.context, operation_id, status);
  }
  std::uint64_t count = 0;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    ExpireActiveEntriesLocked(RequestTime(request.context));
    count = PurgeLocked();
  }
  auto result = SecuritySuccess<EngineShutdownProtectedMaterialResult>(request.context, operation_id);
  result.shutdown_purge_complete = true;
  result.purged_entry_count = count;
  AddSecurityEvidence(&result, "protected_material_shutdown_purge", std::to_string(count));
  AddProtectedMaterialRow(&result,
                          {{"purged_entry_count", std::to_string(count)},
                           {"shutdown_reason", request.shutdown_reason}});
  return result;
}

EngineOpenEncryptedFilespaceResult EngineOpenEncryptedFilespace(
    const EngineOpenEncryptedFilespaceRequest& request) {
  const std::string operation_id = "security.encrypted_filespace.open";
  auto status = RequireSpecificRight(request, operation_id, "PROTECTED_MATERIAL_RELEASE");
  if (status.error) {
    return SecurityFailure<EngineOpenEncryptedFilespaceResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = request.database_uuid.empty()
                                        ? DatabaseUuidFromRequest(request)
                                        : request.database_uuid;
  if (database_uuid.empty() || request.filespace_uuid.empty()) {
    return SecurityFailure<EngineOpenEncryptedFilespaceResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.FILESPACE.OPEN_INVALID",
                               "database_uuid_and_filespace_uuid_required"));
  }

  auto result = SecuritySuccess<EngineOpenEncryptedFilespaceResult>(request.context, operation_id);
  result.encrypted_filespace = request.encrypted_filespace;
  result.database_uuid = database_uuid;
  result.filespace_uuid = request.filespace_uuid;
  result.plaintext_material_returned = false;
  if (!request.encrypted_filespace || !request.decryption_required) {
    result.open_admitted = true;
    AddSecurityEvidence(&result, "filespace_open_unencrypted", request.filespace_uuid);
    return result;
  }

  EngineProtectedMaterialCacheEntry entry;
  bool found = false;
  bool expired = false;
  bool scope_mismatch = false;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    ExpireActiveEntriesLocked(RequestTime(request.context));
    if (!request.key_handle.empty()) {
      if (auto* handle_entry = FindAnyHandleLocked(request.key_handle)) {
        scope_mismatch = handle_entry->database_uuid != database_uuid ||
                         handle_entry->filespace_uuid != request.filespace_uuid ||
                         (!request.key_uuid.empty() && handle_entry->key_uuid != request.key_uuid);
      }
    }
    auto* active = FindEntryLocked(database_uuid,
                                   request.filespace_uuid,
                                   request.key_uuid,
                                   request.key_handle,
                                   true);
    if (active != nullptr) {
      entry = *active;
      found = true;
    } else {
      expired = HasExpiredCandidateLocked(database_uuid,
                                          request.filespace_uuid,
                                          request.key_uuid,
                                          request.key_handle);
    }
  }

  if (!found) {
    result.open_refused = true;
    result.key_expired = expired;
    const auto diagnostic = scope_mismatch
        ? MakeSecurityDiagnostic("SECURITY.KEY.SCOPE_MISMATCH",
                                 "key_handle_not_valid_for_database_or_filespace")
        : (expired
               ? MakeSecurityDiagnostic("SECURITY.KEY.EXPIRED", "key_cache_entry_expired")
               : MakeSecurityDiagnostic("SECURITY.KEY.UNAVAILABLE", "active_key_required_for_encrypted_filespace"));
    auto failure = SecurityFailure<EngineOpenEncryptedFilespaceResult>(request.context, operation_id, diagnostic);
    failure.open_refused = true;
    failure.encrypted_filespace = true;
    failure.database_uuid = database_uuid;
    failure.filespace_uuid = request.filespace_uuid;
    failure.key_expired = expired;
    failure.plaintext_material_returned = false;
    return failure;
  }

  result.open_admitted = true;
  result.key_cache_hit = true;
  result.key_handle = entry.key_handle;
  result.key_generation = entry.generation;
  AddSecurityEvidence(&result, "encrypted_filespace_key_cache_hit", entry.key_uuid);
  AddProtectedMaterialRow(&result,
                          {{"database_uuid", entry.database_uuid},
                           {"filespace_uuid", entry.filespace_uuid},
                           {"key_uuid", entry.key_uuid},
                           {"key_handle", entry.key_handle},
                           {"key_generation", std::to_string(entry.generation)},
                           {"plaintext_material_returned", "false"}});
  return result;
}

EngineRequestProtectedMaterialResult EngineRequestProtectedMaterial(
    const EngineRequestProtectedMaterialRequest& request) {
  const std::string operation_id = "security.request_protected_material";
  auto status = RequireSpecificRight(request, operation_id, "PROTECTED_MATERIAL_RELEASE");
  if (status.error) {
    return SecurityFailure<EngineRequestProtectedMaterialResult>(request.context, operation_id, status);
  }
  const std::string key_state = SecurityOptionValue(request, "key_state:");
  if (key_state == "wrong") {
    return SecurityFailure<EngineRequestProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.KEY.WRONG", "wrong_key"));
  }
  if (key_state == "unavailable") {
    return SecurityFailure<EngineRequestProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.KEY.UNAVAILABLE", "key_unavailable"));
  }
  const std::string purpose = !request.purpose.empty() ? request.purpose : SecurityOptionValue(request, "purpose:");
  if (purpose.empty()) {
    return SecurityFailure<EngineRequestProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.DENIED", "purpose_required"));
  }
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    ExpireActiveEntriesLocked(RequestTime(request.context));
    if (!request.key_handle.empty() &&
        FindEntryLocked({}, {}, {}, request.key_handle, true) == nullptr) {
      return SecurityFailure<EngineRequestProtectedMaterialResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.KEY.UNAVAILABLE", "key_handle_not_active"));
    }
  }
  const auto evidence = AppendSecurityEvidenceEvent(request.context,
                                                    operation_id,
                                                    "protected_material_release",
                                                    RedactProtectedMaterialForDiagnostics(purpose));
  if (evidence.error) {
    return SecurityFailure<EngineRequestProtectedMaterialResult>(request.context, operation_id, evidence);
  }
  auto result = SecuritySuccess<EngineRequestProtectedMaterialResult>(request.context, operation_id);
  result.released = true;
  result.redaction_applied = true;
  result.plaintext_material_returned = false;
  result.protected_material_ref = request.key_handle.empty()
                                      ? "protected-material-ref:redacted"
                                      : request.key_handle;
  AddSecurityEvidence(&result, "protected_material_release", RedactProtectedMaterialForDiagnostics(purpose));
  AddProtectedMaterialRow(&result,
                          {{"purpose", purpose},
                           {"released", "true"},
                           {"protected_material_ref", result.protected_material_ref},
                           {"protected_material", "<protected-material-redacted>"},
                           {"plaintext_material_returned", "false"}});
  return result;
}

EngineCreateProtectedMaterialResult EngineCreateProtectedMaterial(
    const EngineCreateProtectedMaterialRequest& request) {
  const std::string operation_id = "security.protected_material.create";
  auto status = RequireSpecificRight(request, operation_id, "KEY_RELEASE_APPROVE");
  if (status.error) {
    return SecurityFailure<EngineCreateProtectedMaterialResult>(request.context, operation_id, status);
  }
  status = ValidateMutationContext(request, operation_id);
  if (status.error) {
    return SecurityFailure<EngineCreateProtectedMaterialResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (!IsUuidText(request.protected_material_uuid) ||
      !IsKnownPurposeClass(request.purpose_class) ||
      !IsKnownStorageClass(request.storage_class)) {
    return SecurityFailure<EngineCreateProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID",
                               "material_uuid_purpose_storage_required"));
  }
  EngineProtectedMaterialPolicySet policy = request.policy;
  if (policy.release_purposes.empty()) { policy.release_purposes.push_back(request.purpose_class); }
  if (!PolicySetComplete(policy)) {
    return SecurityFailure<EngineCreateProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("CATALOG.PROTECTED_MATERIAL_POLICY_INVALID",
                               "retention_access_release_purge_audit_policy_uuids_required"));
  }
  const bool initial_payload_present = ProtectedPayloadPresent(request.protected_reference,
                                                              request.envelope_reference,
                                                              request.payload_hash);
  if (initial_payload_present && !IsUuidText(request.initial_version_uuid)) {
    return SecurityFailure<EngineCreateProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_INVALID",
                               "initial_version_uuid_required_for_initial_payload"));
  }
  if (!request.initial_version_uuid.empty() && !IsUuidText(request.initial_version_uuid)) {
    return SecurityFailure<EngineCreateProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_INVALID",
                               "protected_material_version_uuid_invalid"));
  }
  if (ProtectedPayloadInputRefused(request.protected_reference,
                                  request.envelope_reference,
                                  request.payload_hash)) {
    return SecurityFailure<EngineCreateProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED",
                               "plaintext_secret_material_is_never_stored"));
  }

  EngineProtectedMaterialCatalogEntry material;
  EngineProtectedMaterialVersionCatalogEntry version;
  EngineProtectedMaterialAuditEvent audit_event;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    status = LoadProtectedMaterialCatalogForDatabaseLocked(request.context, database_uuid);
    if (status.error) {
      return SecurityFailure<EngineCreateProtectedMaterialResult>(request.context, operation_id, status);
    }
    const auto before_mutation = CaptureProtectedMaterialCatalogImageLocked(database_uuid);
    if (FindMaterialLocked(database_uuid, request.protected_material_uuid) != nullptr) {
      return SecurityFailure<EngineCreateProtectedMaterialResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.ALREADY_EXISTS",
                                 "protected_material_uuid_duplicate"));
    }

    const std::uint64_t generation = ++MaterialCatalogGenerationCounter();
    material.database_uuid = database_uuid;
    material.protected_material_uuid = request.protected_material_uuid;
    material.object_class = request.object_class.empty() ? "protected_material" : request.object_class;
    material.owner_scope_uuid = !request.owner_scope_uuid.empty()
        ? request.owner_scope_uuid
        : (!request.context.principal_uuid.canonical.empty()
               ? request.context.principal_uuid.canonical
               : database_uuid);
    material.purpose_class = request.purpose_class;
    material.storage_class = request.storage_class;
    material.lifecycle_state = "active";
    material.policy = policy;
    material.catalog_generation_id = generation;
    material.created_local_transaction_id = MutationTransactionId(request.context);
    material.updated_local_transaction_id = MutationTransactionId(request.context);
    material.security_epoch = request.context.security_epoch;

    if (initial_payload_present) {
      version.database_uuid = database_uuid;
      version.protected_material_uuid = request.protected_material_uuid;
      version.protected_material_version_uuid = request.initial_version_uuid;
      version.version_number = 1;
      version.protected_reference = request.protected_reference;
      version.envelope_reference = request.envelope_reference;
      version.payload_hash = request.payload_hash;
      version.storage_class = request.storage_class;
      version.rotation_state = "active";
      version.policy = policy;
      version.valid_from_local_transaction_id = MutationTransactionId(request.context);
      version.catalog_generation_id = generation;
      version.security_epoch = request.context.security_epoch;
      version.active = true;
      material.active_version_uuid = version.protected_material_version_uuid;
      MaterialVersions().push_back(version);
    }

    MaterialCatalog().push_back(material);
    audit_event = AppendMaterialAuditLocked(request.context,
                                           database_uuid,
                                           request.protected_material_uuid,
                                           material.active_version_uuid,
                                           "create",
                                           "allow",
                                           {},
                                           "protected_material_catalog_create",
                                           generation);
    status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                          database_uuid,
                                                          before_mutation);
    if (status.error) {
      return SecurityFailure<EngineCreateProtectedMaterialResult>(request.context, operation_id, status);
    }
  }

  auto result = SecuritySuccess<EngineCreateProtectedMaterialResult>(request.context, operation_id);
  result.created = true;
  result.initial_version_created = initial_payload_present;
  result.plaintext_material_stored = false;
  result.protected_material_redacted = true;
  result.protected_material_uuid = material.protected_material_uuid;
  result.active_version_uuid = material.active_version_uuid;
  result.active_version_number = initial_payload_present ? 1 : 0;
  AddSecurityEvidence(&result, "protected_material_create", material.protected_material_uuid);
  AddMaterialAuditEvidence(&result, audit_event);
  AddMaterialCatalogResultRow(&result, material, "create");
  if (initial_payload_present) { AddVersionCatalogResultRow(&result, version, "create_initial_version"); }
  AddAuditCatalogResultRow(&result, audit_event);
  return result;
}

EngineAddProtectedMaterialVersionResult EngineAddProtectedMaterialVersion(
    const EngineAddProtectedMaterialVersionRequest& request) {
  const std::string operation_id = "security.protected_material.version.add";
  auto status = RequireSpecificRight(request, operation_id, "KEY_RELEASE_APPROVE");
  if (status.error) {
    return SecurityFailure<EngineAddProtectedMaterialVersionResult>(request.context, operation_id, status);
  }
  status = ValidateMutationContext(request, operation_id);
  if (status.error) {
    return SecurityFailure<EngineAddProtectedMaterialVersionResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (!IsUuidText(request.protected_material_uuid) ||
      !IsUuidText(request.protected_material_version_uuid) ||
      !ProtectedPayloadPresent(request.protected_reference,
                               request.envelope_reference,
                               request.payload_hash)) {
    return SecurityFailure<EngineAddProtectedMaterialVersionResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_INVALID",
                               "material_uuid_version_uuid_and_protected_reference_required"));
  }
  if (ProtectedPayloadInputRefused(request.protected_reference,
                                  request.envelope_reference,
                                  request.payload_hash)) {
    return SecurityFailure<EngineAddProtectedMaterialVersionResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED",
                               "plaintext_secret_material_is_never_stored"));
  }

  EngineProtectedMaterialCatalogEntry material_snapshot;
  EngineProtectedMaterialVersionCatalogEntry version;
  EngineProtectedMaterialAuditEvent audit_event;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    status = LoadProtectedMaterialCatalogForDatabaseLocked(request.context, database_uuid);
    if (status.error) {
      return SecurityFailure<EngineAddProtectedMaterialVersionResult>(request.context, operation_id, status);
    }
    const auto before_mutation = CaptureProtectedMaterialCatalogImageLocked(database_uuid);
    auto* material = FindMaterialLocked(database_uuid, request.protected_material_uuid);
    if (material == nullptr || material->purged) {
      return SecurityFailure<EngineAddProtectedMaterialVersionResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.NOT_FOUND",
                                 "protected_material_not_found"));
    }
    if (FindVersionLocked(database_uuid,
                          request.protected_material_uuid,
                          request.protected_material_version_uuid) != nullptr) {
      return SecurityFailure<EngineAddProtectedMaterialVersionResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_DUPLICATE",
                                 "protected_material_version_uuid_duplicate"));
    }
    const std::string storage_class = request.storage_class.empty()
        ? material->storage_class
        : request.storage_class;
    if (!IsKnownStorageClass(storage_class)) {
      return SecurityFailure<EngineAddProtectedMaterialVersionResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID",
                                 "storage_class_invalid"));
    }
    EngineProtectedMaterialPolicySet policy = MergePolicy(material->policy, request.policy_override);
    if (policy.release_purposes.empty()) { policy.release_purposes.push_back(material->purpose_class); }
    if (!PolicySetComplete(policy)) {
      return SecurityFailure<EngineAddProtectedMaterialVersionResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("CATALOG.PROTECTED_MATERIAL_POLICY_INVALID",
                                 "version_policy_incomplete"));
    }

    const std::uint64_t tx = MutationTransactionId(request.context);
    const std::uint64_t generation = ++MaterialCatalogGenerationCounter();
    for (auto& candidate : MaterialVersions()) {
      if (candidate.database_uuid == database_uuid &&
          candidate.protected_material_uuid == request.protected_material_uuid &&
          candidate.valid_until_local_transaction_id == 0 &&
          !candidate.purged) {
        candidate.valid_until_local_transaction_id = tx;
        candidate.active = false;
        candidate.rotation_state = "rotated";
      }
    }

    version.database_uuid = database_uuid;
    version.protected_material_uuid = request.protected_material_uuid;
    version.protected_material_version_uuid = request.protected_material_version_uuid;
    version.version_number = NextVersionNumberLocked(database_uuid, request.protected_material_uuid);
    version.protected_reference = request.protected_reference;
    version.envelope_reference = request.envelope_reference;
    version.payload_hash = request.payload_hash;
    version.storage_class = storage_class;
    version.rotation_state = "active";
    version.policy = policy;
    version.valid_from_local_transaction_id = tx;
    version.catalog_generation_id = generation;
    version.security_epoch = request.context.security_epoch;
    version.active = true;
    MaterialVersions().push_back(version);

    material->active_version_uuid = version.protected_material_version_uuid;
    material->updated_local_transaction_id = tx;
    material->catalog_generation_id = generation;
    material_snapshot = *material;
    audit_event = AppendMaterialAuditLocked(request.context,
                                           database_uuid,
                                           request.protected_material_uuid,
                                           version.protected_material_version_uuid,
                                           "add_version",
                                           "allow",
                                           {},
                                           request.rotation_reason,
                                           generation);
    status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                          database_uuid,
                                                          before_mutation);
    if (status.error) {
      return SecurityFailure<EngineAddProtectedMaterialVersionResult>(request.context, operation_id, status);
    }
  }

  auto result = SecuritySuccess<EngineAddProtectedMaterialVersionResult>(request.context, operation_id);
  result.version_added = true;
  result.active_version_changed = true;
  result.plaintext_material_stored = false;
  result.protected_material_redacted = true;
  result.protected_material_uuid = material_snapshot.protected_material_uuid;
  result.active_version_uuid = version.protected_material_version_uuid;
  result.active_version_number = version.version_number;
  AddSecurityEvidence(&result, "protected_material_version_add", version.protected_material_version_uuid);
  AddMaterialAuditEvidence(&result, audit_event);
  AddMaterialCatalogResultRow(&result, material_snapshot, "add_version");
  AddVersionCatalogResultRow(&result, version, "add_version");
  AddAuditCatalogResultRow(&result, audit_event);
  return result;
}

EngineResolveProtectedMaterialResult EngineResolveProtectedMaterial(
    const EngineResolveProtectedMaterialRequest& request) {
  const std::string operation_id = "security.protected_material.resolve";
  auto status = RequireSpecificRight(request, operation_id, "PROTECTED_MATERIAL_RELEASE");
  if (status.error) {
    return SecurityFailure<EngineResolveProtectedMaterialResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (database_uuid.empty() || !IsUuidText(request.protected_material_uuid)) {
    return SecurityFailure<EngineResolveProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID",
                               "database_uuid_and_material_uuid_required"));
  }

  EngineProtectedMaterialCatalogEntry material_snapshot;
  EngineProtectedMaterialVersionCatalogEntry version_snapshot;
  EngineProtectedMaterialAuditEvent audit_event;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    status = LoadProtectedMaterialCatalogForDatabaseLocked(request.context, database_uuid);
    if (status.error) {
      return SecurityFailure<EngineResolveProtectedMaterialResult>(request.context, operation_id, status);
    }
    const auto before_mutation = CaptureProtectedMaterialCatalogImageLocked(database_uuid);
    auto* material = FindMaterialLocked(database_uuid, request.protected_material_uuid);
    if (material == nullptr || material->purged) {
      return SecurityFailure<EngineResolveProtectedMaterialResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.NOT_FOUND",
                                 "protected_material_not_found"));
    }
    auto* version = ResolveActiveVersionLocked(request.context,
                                               database_uuid,
                                               request.protected_material_uuid);
    if (version == nullptr) {
      audit_event = AppendMaterialAuditLocked(request.context,
                                             database_uuid,
                                             request.protected_material_uuid,
                                             {},
                                             "resolve",
                                             "deny",
                                             "SECURITY.PROTECTED_MATERIAL.VERSION_NOT_VISIBLE",
                                             "active_version_not_visible",
                                             ++MaterialCatalogGenerationCounter());
      status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                            database_uuid,
                                                            before_mutation);
      if (status.error) {
        return SecurityFailure<EngineResolveProtectedMaterialResult>(request.context, operation_id, status);
      }
      auto failure = SecurityFailure<EngineResolveProtectedMaterialResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_NOT_VISIBLE",
                                 "active_version_not_visible"));
      AddMaterialAuditEvidence(&failure, audit_event);
      AddAuditCatalogResultRow(&failure, audit_event);
      return failure;
    }
    material_snapshot = *material;
    version_snapshot = RedactedVersion(*version);
    audit_event = AppendMaterialAuditLocked(request.context,
                                           database_uuid,
                                           request.protected_material_uuid,
                                           version->protected_material_version_uuid,
                                           "resolve",
                                           "allow",
                                           {},
                                           request.purpose,
                                           ++MaterialCatalogGenerationCounter());
    status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                          database_uuid,
                                                          before_mutation);
    if (status.error) {
      return SecurityFailure<EngineResolveProtectedMaterialResult>(request.context, operation_id, status);
    }
  }

  auto result = SecuritySuccess<EngineResolveProtectedMaterialResult>(request.context, operation_id);
  result.resolved = true;
  result.active_version_visible = true;
  result.protected_material_redacted = true;
  result.protected_material_uuid = material_snapshot.protected_material_uuid;
  result.protected_material_version_uuid = version_snapshot.protected_material_version_uuid;
  result.version_number = version_snapshot.version_number;
  result.protected_material_ref = ProtectedMaterialRefFor(version_snapshot);
  AddSecurityEvidence(&result, "protected_material_resolve", version_snapshot.protected_material_version_uuid);
  AddMaterialAuditEvidence(&result, audit_event);
  AddMaterialCatalogResultRow(&result, material_snapshot, "resolve");
  AddVersionCatalogResultRow(&result, version_snapshot, "resolve");
  AddAuditCatalogResultRow(&result, audit_event);
  return result;
}

EngineReleaseProtectedMaterialResult EngineReleaseProtectedMaterial(
    const EngineReleaseProtectedMaterialRequest& request) {
  const std::string operation_id = "security.protected_material.release";
  auto status = RequireSpecificRight(request, operation_id, "PROTECTED_MATERIAL_RELEASE");
  if (status.error) {
    return SecurityFailure<EngineReleaseProtectedMaterialResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (database_uuid.empty() || !IsUuidText(request.protected_material_uuid) ||
      request.purpose.empty()) {
    return SecurityFailure<EngineReleaseProtectedMaterialResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID",
                               "database_uuid_material_uuid_and_purpose_required"));
  }

  EngineProtectedMaterialCatalogEntry material_snapshot;
  EngineProtectedMaterialVersionCatalogEntry version_snapshot;
  EngineProtectedMaterialAuditEvent audit_event;
  std::string release_handle;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    status = LoadProtectedMaterialCatalogForDatabaseLocked(request.context, database_uuid);
    if (status.error) {
      return SecurityFailure<EngineReleaseProtectedMaterialResult>(request.context, operation_id, status);
    }
    const auto before_mutation = CaptureProtectedMaterialCatalogImageLocked(database_uuid);
    auto* material = FindMaterialLocked(database_uuid, request.protected_material_uuid);
    if (material == nullptr || material->purged) {
      return SecurityFailure<EngineReleaseProtectedMaterialResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.NOT_FOUND",
                                 "protected_material_not_found"));
    }
    EngineProtectedMaterialVersionCatalogEntry* version = nullptr;
    if (!request.protected_material_version_uuid.empty()) {
      version = FindVersionLocked(database_uuid,
                                  request.protected_material_uuid,
                                  request.protected_material_version_uuid);
      if (version != nullptr &&
          !VersionVisibleAt(*version, ReadVisibilityPoint(request.context))) {
        version = nullptr;
      }
    } else {
      version = ResolveActiveVersionLocked(request.context,
                                           database_uuid,
                                           request.protected_material_uuid);
    }
    if (version == nullptr) {
      return SecurityFailure<EngineReleaseProtectedMaterialResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_NOT_VISIBLE",
                                 "version_not_visible"));
    }
    const std::uint64_t generation = ++MaterialCatalogGenerationCounter();
    if (!PurposeAllowed(*version, *material, request.purpose)) {
      audit_event = AppendMaterialAuditLocked(request.context,
                                             database_uuid,
                                             request.protected_material_uuid,
                                             version->protected_material_version_uuid,
                                             "release",
                                             "deny",
                                             "SECURITY.PROTECTED_MATERIAL.POLICY_DENIED",
                                             request.purpose,
                                             generation);
      status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                            database_uuid,
                                                            before_mutation);
      if (status.error) {
        return SecurityFailure<EngineReleaseProtectedMaterialResult>(request.context, operation_id, status);
      }
      auto failure = SecurityFailure<EngineReleaseProtectedMaterialResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.POLICY_DENIED",
                                 "release_purpose_not_allowed"));
      failure.policy_denied = true;
      failure.plaintext_material_returned = false;
      failure.audit_event_uuid = audit_event.audit_event_uuid;
      AddMaterialAuditEvidence(&failure, audit_event);
      AddAuditCatalogResultRow(&failure, audit_event);
      return failure;
    }

    material_snapshot = *material;
    version_snapshot = RedactedVersion(*version);
    release_handle = ReleaseHandleFor(*version, request.purpose, generation);
    audit_event = AppendMaterialAuditLocked(request.context,
                                           database_uuid,
                                           request.protected_material_uuid,
                                           version->protected_material_version_uuid,
                                           "release",
                                           "allow",
                                           {},
                                           request.purpose,
                                           generation);
    status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                          database_uuid,
                                                          before_mutation);
    if (status.error) {
      return SecurityFailure<EngineReleaseProtectedMaterialResult>(request.context, operation_id, status);
    }
  }

  auto result = SecuritySuccess<EngineReleaseProtectedMaterialResult>(request.context, operation_id);
  result.released = true;
  result.policy_denied = false;
  result.redaction_applied = true;
  result.plaintext_material_returned = false;
  result.protected_material_uuid = material_snapshot.protected_material_uuid;
  result.protected_material_version_uuid = version_snapshot.protected_material_version_uuid;
  result.release_handle = release_handle;
  result.audit_event_uuid = audit_event.audit_event_uuid;
  AddSecurityEvidence(&result, "protected_material_release", version_snapshot.protected_material_version_uuid);
  AddMaterialAuditEvidence(&result, audit_event);
  AddMaterialCatalogResultRow(&result, material_snapshot, "release");
  AddVersionCatalogResultRow(&result, version_snapshot, "release");
  AddAuditCatalogResultRow(&result, audit_event);
  return result;
}

EnginePurgeProtectedMaterialVersionResult EnginePurgeProtectedMaterialVersion(
    const EnginePurgeProtectedMaterialVersionRequest& request) {
  const std::string operation_id = "security.protected_material.version.purge";
  auto status = RequireSpecificRight(request, operation_id, "KEY_RELEASE_APPROVE");
  if (status.error) {
    return SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(request.context, operation_id, status);
  }
  status = ValidateMutationContext(request, operation_id);
  if (status.error) {
    return SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (!IsUuidText(request.protected_material_uuid) ||
      !IsUuidText(request.protected_material_version_uuid)) {
    return SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_INVALID",
                               "material_uuid_and_version_uuid_required"));
  }

  EngineProtectedMaterialCatalogEntry material_snapshot;
  EngineProtectedMaterialVersionCatalogEntry version_snapshot;
  EngineProtectedMaterialAuditEvent audit_event;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    status = LoadProtectedMaterialCatalogForDatabaseLocked(request.context, database_uuid);
    if (status.error) {
      return SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(request.context, operation_id, status);
    }
    const auto before_mutation = CaptureProtectedMaterialCatalogImageLocked(database_uuid);
    auto* material = FindMaterialLocked(database_uuid, request.protected_material_uuid);
    auto* version = FindVersionLocked(database_uuid,
                                      request.protected_material_uuid,
                                      request.protected_material_version_uuid);
    if (material == nullptr || version == nullptr) {
      return SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.VERSION_NOT_FOUND",
                                 "protected_material_version_not_found"));
    }
    const std::uint64_t now = RequestTime(request.context);
    const std::uint64_t generation = ++MaterialCatalogGenerationCounter();
    if (version->policy.legal_hold ||
        (version->policy.retention_until_epoch_millis != 0 &&
         version->policy.retention_until_epoch_millis > now)) {
      audit_event = AppendMaterialAuditLocked(request.context,
                                             database_uuid,
                                             request.protected_material_uuid,
                                             request.protected_material_version_uuid,
                                             "purge",
                                             "deny",
                                             "SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED",
                                             request.purge_reason,
                                             generation);
      status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                            database_uuid,
                                                            before_mutation);
      if (status.error) {
        return SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(request.context, operation_id, status);
      }
      auto failure = SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(
          request.context,
          operation_id,
          MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED",
                                 "retention_or_legal_hold_blocks_purge"));
      failure.refused_by_retention = true;
      failure.audit_preserved = true;
      failure.protected_reference_reachable = !version->protected_reference.empty() ||
                                             !version->envelope_reference.empty();
      failure.protected_material_uuid = request.protected_material_uuid;
      failure.protected_material_version_uuid = request.protected_material_version_uuid;
      failure.audit_event_uuid = audit_event.audit_event_uuid;
      AddMaterialAuditEvidence(&failure, audit_event);
      AddAuditCatalogResultRow(&failure, audit_event);
      return failure;
    }

    const std::uint64_t tx = MutationTransactionId(request.context);
    version->protected_reference.clear();
    version->envelope_reference.clear();
    version->active = false;
    version->purged = true;
    version->rotation_state = "purged";
    version->valid_until_local_transaction_id = tx;
    version->catalog_generation_id = generation;
    if (material->active_version_uuid == version->protected_material_version_uuid) {
      EngineProtectedMaterialVersionCatalogEntry* replacement = nullptr;
      for (auto& candidate : MaterialVersions()) {
        if (candidate.database_uuid != database_uuid ||
            candidate.protected_material_uuid != request.protected_material_uuid ||
            candidate.purged) {
          continue;
        }
        if (replacement == nullptr || candidate.version_number > replacement->version_number) {
          replacement = &candidate;
        }
      }
      material->active_version_uuid = replacement == nullptr
          ? std::string{}
          : replacement->protected_material_version_uuid;
      material->lifecycle_state = replacement == nullptr ? "retained_no_active_version" : "active";
    }
    material->updated_local_transaction_id = tx;
    material->catalog_generation_id = generation;
    material_snapshot = *material;
    version_snapshot = *version;
    audit_event = AppendMaterialAuditLocked(request.context,
                                           database_uuid,
                                           request.protected_material_uuid,
                                           request.protected_material_version_uuid,
                                           "purge",
                                           "allow",
                                           {},
                                           request.purge_reason,
                                           generation);
    status = PersistProtectedMaterialCatalogMutationLocked(request.context,
                                                          database_uuid,
                                                          before_mutation);
    if (status.error) {
      return SecurityFailure<EnginePurgeProtectedMaterialVersionResult>(request.context, operation_id, status);
    }
  }

  auto result = SecuritySuccess<EnginePurgeProtectedMaterialVersionResult>(request.context, operation_id);
  result.purged = true;
  result.refused_by_retention = false;
  result.audit_preserved = true;
  result.protected_reference_reachable = false;
  result.protected_material_uuid = material_snapshot.protected_material_uuid;
  result.protected_material_version_uuid = version_snapshot.protected_material_version_uuid;
  result.audit_event_uuid = audit_event.audit_event_uuid;
  AddSecurityEvidence(&result, "protected_material_purge", version_snapshot.protected_material_version_uuid);
  AddMaterialAuditEvidence(&result, audit_event);
  AddMaterialCatalogResultRow(&result, material_snapshot, "purge");
  AddVersionCatalogResultRow(&result, version_snapshot, "purge");
  AddAuditCatalogResultRow(&result, audit_event);
  return result;
}

EngineInspectProtectedMaterialCatalogResult EngineInspectProtectedMaterialCatalog(
    const EngineInspectProtectedMaterialCatalogRequest& request) {
  const std::string operation_id = "security.protected_material.catalog.inspect";
  auto status = RequireSpecificRight(request, operation_id, "PROTECTED_MATERIAL_RELEASE");
  if (status.error) {
    return SecurityFailure<EngineInspectProtectedMaterialCatalogResult>(request.context, operation_id, status);
  }
  const std::string database_uuid = DatabaseUuidFromRequest(request);
  if (database_uuid.empty()) {
    return SecurityFailure<EngineInspectProtectedMaterialCatalogResult>(
        request.context,
        operation_id,
        MakeSecurityDiagnostic("SECURITY.PROTECTED_MATERIAL.CATALOG_INVALID",
                               "database_uuid_required"));
  }
  auto result = SecuritySuccess<EngineInspectProtectedMaterialCatalogResult>(request.context, operation_id);
  result.protected_material_redacted = true;
  {
    std::lock_guard<std::mutex> lock(CacheMutex());
    status = LoadProtectedMaterialCatalogForDatabaseLocked(request.context, database_uuid);
    if (status.error) {
      return SecurityFailure<EngineInspectProtectedMaterialCatalogResult>(request.context, operation_id, status);
    }
    for (const auto& material : MaterialCatalog()) {
      if (!database_uuid.empty() && material.database_uuid != database_uuid) { continue; }
      if (!request.protected_material_uuid.empty() &&
          material.protected_material_uuid != request.protected_material_uuid) {
        continue;
      }
      result.materials.push_back(material);
      AddMaterialCatalogResultRow(&result, material, "inspect");
    }
    if (request.include_versions) {
      for (const auto& version : MaterialVersions()) {
        if (!database_uuid.empty() && version.database_uuid != database_uuid) { continue; }
        if (!request.protected_material_uuid.empty() &&
            version.protected_material_uuid != request.protected_material_uuid) {
          continue;
        }
        result.versions.push_back(RedactedVersion(version));
        AddVersionCatalogResultRow(&result, RedactedVersion(version), "inspect");
      }
    }
    if (request.include_audit) {
      for (const auto& event : MaterialAuditEvents()) {
        if (!database_uuid.empty() && event.database_uuid != database_uuid) { continue; }
        if (!request.protected_material_uuid.empty() &&
            event.protected_material_uuid != request.protected_material_uuid) {
          continue;
        }
        result.audit_events.push_back(event);
        AddAuditCatalogResultRow(&result, event);
      }
    }
  }
  AddSecurityEvidence(&result, "protected_material_catalog_inspect",
                      std::to_string(result.materials.size()));
  return result;
}

}  // namespace scratchbird::engine::internal_api
