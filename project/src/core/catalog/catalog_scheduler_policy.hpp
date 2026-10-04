// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "uuid.hpp"
#include <array>
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {

// SB-MGA-SCHED-TYPED-RUNTIME-POLICY-V1. Definition data, not selected policy,
// a generation pin, resource capability, activation or a restart receipt.
struct CatalogSchedulerPolicy {
  Uuid policy_uuid, database_uuid;
  Uuid queue_profile_uuid, resource_profile_uuid, fairness_profile_uuid;
  u64 generation = 0;
  u64 generation_maximum = 0;
  u64 generation_warning_threshold = 0;
  u64 generation_admission_stop_threshold = 0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};

struct CatalogSchedulerPolicyResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogSchedulerPolicy> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};

inline bool ValidCatalogSchedulerPolicy(const CatalogSchedulerPolicy& r) noexcept {
  for (const auto* id : {&r.policy_uuid, &r.database_uuid, &r.queue_profile_uuid,
                        &r.resource_profile_uuid, &r.fairness_profile_uuid})
    if (!uuid::IsEngineIdentityUuid(*id)) return false;
  return r.generation != 0 && r.origin_local_transaction_id != 0 &&
      r.origin_transaction_uuid.kind == UuidKind::transaction &&
      uuid::IsEngineIdentityUuid(r.origin_transaction_uuid.value) &&
      r.generation_warning_threshold <= r.generation_admission_stop_threshold &&
      r.generation_admission_stop_threshold < r.generation_maximum;
}

inline constexpr CatalogValueSchemaView CatalogSchedulerPolicySchemaView() noexcept {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
      {1,T::engine_identity,true,16,UuidKind::object},
      {2,T::unsigned_integer,true,8},
      {3,T::engine_identity,true,16,UuidKind::database},
      {4,T::engine_identity,true,16,UuidKind::object},
      {5,T::engine_identity,true,16,UuidKind::object},
      {6,T::engine_identity,true,16,UuidKind::object},
      {7,T::unsigned_integer,true,8}, {8,T::unsigned_integer,true,8},
      {9,T::unsigned_integer,true,8},
      {10,T::engine_identity,true,16,UuidKind::transaction},
      {11,T::unsigned_integer,true,8}};
  return {786433, 1, fields};
}

// Compatibility owning encoder: may allocate and throw. It must not be called
// under a source/device fence or mistaken for a governed mutation operation.
inline CatalogValueEncodeResult EncodeCatalogSchedulerPolicy(const CatalogSchedulerPolicy& r) {
  if (!ValidCatalogSchedulerPolicy(r)) return {CatalogValueError::invalid_value, {}};
  const auto view = CatalogSchedulerPolicySchemaView();
  const CatalogValueSchema schema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  return EncodeCatalogValueBlock(schema, {
      {1,TypedUuid{UuidKind::object,r.policy_uuid}}, {2,r.generation},
      {3,TypedUuid{UuidKind::database,r.database_uuid}},
      {4,TypedUuid{UuidKind::object,r.queue_profile_uuid}},
      {5,TypedUuid{UuidKind::object,r.resource_profile_uuid}},
      {6,TypedUuid{UuidKind::object,r.fairness_profile_uuid}},
      {7,r.generation_maximum}, {8,r.generation_warning_threshold},
      {9,r.generation_admission_stop_threshold},
      {10,r.origin_transaction_uuid}, {11,r.origin_local_transaction_id}});
}

// Complete structural decoding into fixed native values. No borrowed payload
// escapes; no input clone, dynamic schema, re-encoding or heap-backed scratch.
inline CatalogSchedulerPolicyResult DecodeCatalogSchedulerPolicy(std::string_view bytes) {
  if (bytes.size() != 248) return {CatalogValueError::invalid_framing, {}};
  std::array<CatalogValueFieldView, 11> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogSchedulerPolicySchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  const auto& f = decoded.fields;
  CatalogSchedulerPolicy r;
  r.policy_uuid = f[0].identity()->value;
  r.generation = *f[1].unsigned_value();
  r.database_uuid = f[2].identity()->value;
  r.queue_profile_uuid = f[3].identity()->value;
  r.resource_profile_uuid = f[4].identity()->value;
  r.fairness_profile_uuid = f[5].identity()->value;
  r.generation_maximum = *f[6].unsigned_value();
  r.generation_warning_threshold = *f[7].unsigned_value();
  r.generation_admission_stop_threshold = *f[8].unsigned_value();
  r.origin_transaction_uuid = *f[9].identity();
  r.origin_local_transaction_id = *f[10].unsigned_value();
  if (!ValidCatalogSchedulerPolicy(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, r};
}

inline bool IsCatalogSchedulerPolicyPayload(std::string_view bytes) noexcept {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16) == 786433;
}

inline bool CatalogSchedulerPolicyMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::policy ||
      r.header.object_uuid.kind != UuidKind::object ||
      !uuid::IsEngineIdentityUuid(r.header.object_uuid.value)) return false;
  const auto decoded = DecodeCatalogSchedulerPolicy(r.payload);
  return decoded.ok() && decoded.record->policy_uuid == r.header.object_uuid.value;
}
inline bool CatalogSchedulerPolicyMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogSchedulerPolicyMatchesHeader(BorrowCatalogTypedRecord(r));
}

inline bool CatalogSchedulerPolicyMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto identity = [](const TypedUuid& id, UuidKind kind) {
    return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
  };
  if (!CatalogSchedulerPolicyMatchesHeader(m.record) || m.object_subtype != "scheduler_runtime" ||
      m.authority_scope != CatalogAuthorityScope::local ||
      !identity(m.owning_schema_uuid, UuidKind::schema) ||
      !identity(m.record.header.parent_uuid, UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !identity(m.default_name_uuid, UuidKind::object) ||
      !identity(m.name_vector_uuid, UuidKind::object) ||
      !identity(m.creator_transaction_uuid, UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogSchedulerPolicy(m.record.payload);
  const auto& r = *decoded.record;
  return r.generation == m.definition_version &&
      (m.definition_version != 1 ||
       (r.origin_transaction_uuid.value == m.creator_transaction_uuid.value &&
        r.origin_local_transaction_id == m.creator_local_transaction_id));
}
inline bool CatalogSchedulerPolicyMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogSchedulerPolicyMatchesMetadata(BorrowCatalogMetadataVersion(m));
}

inline bool CatalogSchedulerPolicyPreservesOrigin(
    const CatalogMetadataVersionView& previous, const CatalogMetadataVersionView& successor) {
  const auto family = [](const auto& m) {
    return m.object_subtype == "scheduler_runtime" || IsCatalogSchedulerPolicyPayload(m.record.payload);
  };
  if (!family(previous) && !family(successor)) return true;
  if (!CatalogSchedulerPolicyMatchesMetadata(previous) ||
      !CatalogSchedulerPolicyMatchesMetadata(successor)) return false;
  const auto a = DecodeCatalogSchedulerPolicy(previous.record.payload);
  const auto b = DecodeCatalogSchedulerPolicy(successor.record.payload);
  return a.record->policy_uuid == b.record->policy_uuid &&
      a.record->database_uuid == b.record->database_uuid &&
      a.record->origin_transaction_uuid.value == b.record->origin_transaction_uuid.value &&
      a.record->origin_local_transaction_id == b.record->origin_local_transaction_id;
}
inline bool CatalogSchedulerPolicyPreservesOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor) {
  return CatalogSchedulerPolicyPreservesOrigin(
      BorrowCatalogMetadataVersion(previous), BorrowCatalogMetadataVersion(successor));
}

// SB-MGA-SCHED-TYPED-QUEUE-PROFILE-V1. Slots follow the twelve canonical queue
// names, not an implementation hash order or names supplied by an untrusted row.
struct CatalogSchedulerQueueLimits {
  u64 max_depth = 0, max_concurrent = 0;
  Uuid admission_policy_uuid, fairness_profile_uuid;
};
struct CatalogSchedulerQueueProfile {
  Uuid profile_uuid, database_uuid;
  u64 generation = 0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  std::array<CatalogSchedulerQueueLimits,12> queues;
};
struct CatalogSchedulerQueueProfileResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogSchedulerQueueProfile> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};
inline bool ValidCatalogSchedulerQueueProfile(const CatalogSchedulerQueueProfile& r) noexcept {
  if (!uuid::IsEngineIdentityUuid(r.profile_uuid) || !uuid::IsEngineIdentityUuid(r.database_uuid) ||
      !r.generation || !r.origin_local_transaction_id || r.origin_transaction_uuid.kind != UuidKind::transaction ||
      !uuid::IsEngineIdentityUuid(r.origin_transaction_uuid.value)) return false;
  for (const auto& queue : r.queues)
    if (!uuid::IsEngineIdentityUuid(queue.admission_policy_uuid) ||
        !uuid::IsEngineIdentityUuid(queue.fairness_profile_uuid)) return false;
  return true;
}
inline constexpr CatalogValueSchemaView CatalogSchedulerQueueProfileSchemaView() noexcept {
  static constexpr auto fields = [] {
    using T = CatalogValueType;
    std::array<CatalogValueFieldSchema,53> result{};
    result[0] = {1,T::engine_identity,true,16,UuidKind::object};
    result[1] = {2,T::unsigned_integer,true,8};
    result[2] = {3,T::engine_identity,true,16,UuidKind::database};
    result[3] = {4,T::engine_identity,true,16,UuidKind::transaction};
    result[4] = {5,T::unsigned_integer,true,8};
    for (u16 i = 0; i < 12; ++i) {
      const u16 base = static_cast<u16>(16+4*i);
      const std::size_t offset = 5+4*i;
      result[offset] = {base,T::unsigned_integer,true,8};
      result[offset+1] = {static_cast<u16>(base+1),T::unsigned_integer,true,8};
      result[offset+2] = {static_cast<u16>(base+2),T::engine_identity,true,16,UuidKind::object};
      result[offset+3] = {static_cast<u16>(base+3),T::engine_identity,true,16,UuidKind::object};
    }
    return result;
  }();
  return {786434,1,fields};
}
inline CatalogValueEncodeResult EncodeCatalogSchedulerQueueProfile(const CatalogSchedulerQueueProfile& r) {
  if (!ValidCatalogSchedulerQueueProfile(r)) return {CatalogValueError::invalid_value,{}};
  const auto schema = CatalogSchedulerQueueProfileSchemaView();
  std::vector<CatalogValueField> fields;
  fields.reserve(53);
  fields.push_back({1,TypedUuid{UuidKind::object,r.profile_uuid}});
  fields.push_back({2,r.generation});
  fields.push_back({3,TypedUuid{UuidKind::database,r.database_uuid}});
  fields.push_back({4,r.origin_transaction_uuid});
  fields.push_back({5,r.origin_local_transaction_id});
  for (u16 i = 0; i < 12; ++i) {
    const u16 base = static_cast<u16>(16+4*i);
    const auto& q = r.queues[i];
    fields.push_back({base,q.max_depth});
    fields.push_back({static_cast<u16>(base+1),q.max_concurrent});
    fields.push_back({static_cast<u16>(base+2),TypedUuid{UuidKind::object,q.admission_policy_uuid}});
    fields.push_back({static_cast<u16>(base+3),TypedUuid{UuidKind::object,q.fairness_profile_uuid}});
  }
  return EncodeCatalogValueBlock({schema.id,schema.version,{schema.fields.begin(),schema.fields.end()}},fields);
}
inline CatalogSchedulerQueueProfileResult DecodeCatalogSchedulerQueueProfile(std::string_view bytes) {
  if (bytes.size() != 1088) return {CatalogValueError::invalid_framing,{}};
  std::array<CatalogValueFieldView,53> scratch;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogSchedulerQueueProfileSchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},scratch);
  if (!decoded.ok()) return {decoded.error,{}};
  const auto& fields = decoded.fields;
  CatalogSchedulerQueueProfile r;
  r.profile_uuid = fields[0].identity()->value; r.generation = *fields[1].unsigned_value();
  r.database_uuid = fields[2].identity()->value; r.origin_transaction_uuid = *fields[3].identity();
  r.origin_local_transaction_id = *fields[4].unsigned_value();
  for (std::size_t i = 0; i < 12; ++i) {
    auto& q = r.queues[i]; const auto base = 5+4*i;
    q.max_depth = *fields[base].unsigned_value(); q.max_concurrent = *fields[base+1].unsigned_value();
    q.admission_policy_uuid = fields[base+2].identity()->value;
    q.fairness_profile_uuid = fields[base+3].identity()->value;
  }
  if (!ValidCatalogSchedulerQueueProfile(r)) return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,r};
}
inline bool IsCatalogSchedulerQueueProfilePayload(std::string_view bytes) noexcept {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16) == 786434;
}
inline bool CatalogSchedulerQueueProfileMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::policy || r.header.object_uuid.kind != UuidKind::object ||
      !uuid::IsEngineIdentityUuid(r.header.object_uuid.value)) return false;
  const auto decoded = DecodeCatalogSchedulerQueueProfile(r.payload);
  return decoded.ok() && decoded.record->profile_uuid == r.header.object_uuid.value;
}
inline bool CatalogSchedulerQueueProfileMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogSchedulerQueueProfileMatchesHeader(BorrowCatalogTypedRecord(r));
}
inline bool CatalogSchedulerQueueProfileMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto identity = [](const TypedUuid& id, UuidKind kind) {
    return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
  };
  if (!CatalogSchedulerQueueProfileMatchesHeader(m.record) || m.object_subtype != "scheduler_queues" ||
      m.authority_scope != CatalogAuthorityScope::local ||
      !identity(m.owning_schema_uuid,UuidKind::schema) || !identity(m.record.header.parent_uuid,UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !identity(m.default_name_uuid,UuidKind::object) || !identity(m.name_vector_uuid,UuidKind::object) ||
      !identity(m.creator_transaction_uuid,UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogSchedulerQueueProfile(m.record.payload); const auto& r = *decoded.record;
  return r.generation == m.definition_version &&
      (m.definition_version != 1 || (r.origin_transaction_uuid.value == m.creator_transaction_uuid.value &&
                                   r.origin_local_transaction_id == m.creator_local_transaction_id));
}
inline bool CatalogSchedulerQueueProfileMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogSchedulerQueueProfileMatchesMetadata(BorrowCatalogMetadataVersion(m));
}
inline bool CatalogSchedulerQueueProfilePreservesOrigin(
    const CatalogMetadataVersionView& previous, const CatalogMetadataVersionView& successor) {
  const auto family = [](const auto& m) {
    return m.object_subtype == "scheduler_queues" || IsCatalogSchedulerQueueProfilePayload(m.record.payload);
  };
  if (!family(previous) && !family(successor)) return true;
  if (!CatalogSchedulerQueueProfileMatchesMetadata(previous) ||
      !CatalogSchedulerQueueProfileMatchesMetadata(successor)) return false;
  const auto a = DecodeCatalogSchedulerQueueProfile(previous.record.payload);
  const auto b = DecodeCatalogSchedulerQueueProfile(successor.record.payload);
  return a.record->profile_uuid == b.record->profile_uuid && a.record->database_uuid == b.record->database_uuid &&
      a.record->origin_transaction_uuid.value == b.record->origin_transaction_uuid.value &&
      a.record->origin_local_transaction_id == b.record->origin_local_transaction_id;
}
inline bool CatalogSchedulerQueueProfilePreservesOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor) {
  return CatalogSchedulerQueueProfilePreservesOrigin(BorrowCatalogMetadataVersion(previous),BorrowCatalogMetadataVersion(successor));
}
} // namespace scratchbird::core::catalog
