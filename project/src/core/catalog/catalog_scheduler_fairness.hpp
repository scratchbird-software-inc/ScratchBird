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

// SB-MGA-SCHED-TYPED-FAIRNESS-PROFILE-V1. Persisted definition only: these
// limits/references do not constitute current policy, grants or an override.
struct CatalogSchedulerQueueWait {
  u64 maximum_queue_wait_us = 0;
  bool allow_indefinite_deferral = false;
};
struct CatalogSchedulerFairnessProfile {
  Uuid profile_uuid, database_uuid;
  u64 generation = 0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 aging_interval_us = 0, aging_rank_ceiling = 0;
  u64 per_target_max_concurrent = 0;
  u64 foreground_reserved_worker_slots = 0, maintenance_reserved_worker_slots = 0;
  bool emergency_override_allowed = false;
  std::optional<Uuid> emergency_evidence_policy_uuid;
  std::array<u64,24> family_max_concurrent{};
  std::array<u64,9> base_priority_rank{};
  std::array<CatalogSchedulerQueueWait,12> queue_waits{};
};
struct CatalogSchedulerFairnessProfileResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogSchedulerFairnessProfile> record;
  bool ok() const noexcept { return error == CatalogValueError::none && record.has_value(); }
};
inline bool ValidCatalogSchedulerFairnessProfile(const CatalogSchedulerFairnessProfile& r) noexcept {
  if (!uuid::IsEngineIdentityUuid(r.profile_uuid) || !uuid::IsEngineIdentityUuid(r.database_uuid) ||
      !r.generation || !r.origin_local_transaction_id || !r.aging_interval_us ||
      r.origin_transaction_uuid.kind != UuidKind::transaction ||
      !uuid::IsEngineIdentityUuid(r.origin_transaction_uuid.value) ||
      (r.emergency_override_allowed && !r.emergency_evidence_policy_uuid) ||
      (r.emergency_evidence_policy_uuid && !uuid::IsEngineIdentityUuid(*r.emergency_evidence_policy_uuid)))
    return false;
  for (const auto rank : r.base_priority_rank)
    if (rank > r.aging_rank_ceiling) return false;
  return true;
}
inline constexpr CatalogValueSchemaView CatalogSchedulerFairnessProfileSchemaView() noexcept {
  static constexpr auto fields = [] {
    using T = CatalogValueType;
    std::array<CatalogValueFieldSchema,69> f{};
    f[0] = {1,T::engine_identity,true,16,UuidKind::object};
    f[1] = {2,T::unsigned_integer,true,8};
    f[2] = {3,T::engine_identity,true,16,UuidKind::database};
    f[3] = {4,T::engine_identity,true,16,UuidKind::transaction};
    for (u16 i = 4; i < 10; ++i) f[i] = {static_cast<u16>(i+1),T::unsigned_integer,true,8};
    f[10] = {11,T::boolean,true,1};
    f[11] = {12,T::engine_identity,false,16,UuidKind::object};
    for (u16 i = 0; i < 24; ++i) f[12+i] = {static_cast<u16>(16+i),T::unsigned_integer,true,8};
    for (u16 i = 0; i < 9; ++i) f[36+i] = {static_cast<u16>(48+i),T::unsigned_integer,true,8};
    for (u16 i = 0; i < 12; ++i) {
      f[45+2*i] = {static_cast<u16>(64+2*i),T::unsigned_integer,true,8};
      f[46+2*i] = {static_cast<u16>(65+2*i),T::boolean,true,1};
    }
    return f;
  }();
  return {786435,1,fields};
}

// Compatibility encoder can allocate/throw; not a governed writer or suitable
// for use beneath source/device fences. Native decoding below has fixed scratch.
inline CatalogValueEncodeResult EncodeCatalogSchedulerFairnessProfile(const CatalogSchedulerFairnessProfile& r) {
  if (!ValidCatalogSchedulerFairnessProfile(r)) return {CatalogValueError::invalid_value,{}};
  const auto schema = CatalogSchedulerFairnessProfileSchemaView();
  std::vector<CatalogValueField> f;
  f.reserve(69);
  f.push_back({1,TypedUuid{UuidKind::object,r.profile_uuid}});
  f.push_back({2,r.generation});
  f.push_back({3,TypedUuid{UuidKind::database,r.database_uuid}});
  f.push_back({4,r.origin_transaction_uuid});
  f.push_back({5,r.origin_local_transaction_id});
  f.push_back({6,r.aging_interval_us}); f.push_back({7,r.aging_rank_ceiling});
  f.push_back({8,r.per_target_max_concurrent});
  f.push_back({9,r.foreground_reserved_worker_slots});
  f.push_back({10,r.maintenance_reserved_worker_slots});
  f.push_back({11,r.emergency_override_allowed});
  if (r.emergency_evidence_policy_uuid)
    f.push_back({12,TypedUuid{UuidKind::object,*r.emergency_evidence_policy_uuid}});
  for (u16 i = 0; i < 24; ++i) f.push_back({static_cast<u16>(16+i),r.family_max_concurrent[i]});
  for (u16 i = 0; i < 9; ++i) f.push_back({static_cast<u16>(48+i),r.base_priority_rank[i]});
  for (u16 i = 0; i < 12; ++i) {
    f.push_back({static_cast<u16>(64+2*i),r.queue_waits[i].maximum_queue_wait_us});
    f.push_back({static_cast<u16>(65+2*i),r.queue_waits[i].allow_indefinite_deferral});
  }
  return EncodeCatalogValueBlock({schema.id,schema.version,{schema.fields.begin(),schema.fields.end()}},f);
}
inline CatalogSchedulerFairnessProfileResult DecodeCatalogSchedulerFairnessProfile(std::string_view bytes) {
  if (bytes.size() != 1045 && bytes.size() != 1069) return {CatalogValueError::invalid_framing,{}};
  std::array<CatalogValueFieldView,69> scratch;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogSchedulerFairnessProfileSchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},scratch);
  if (!decoded.ok()) return {decoded.error,{}};
  const auto f = decoded.fields;
  CatalogSchedulerFairnessProfile r;
  r.profile_uuid = f[0].identity()->value; r.generation = *f[1].unsigned_value();
  r.database_uuid = f[2].identity()->value; r.origin_transaction_uuid = *f[3].identity();
  r.origin_local_transaction_id = *f[4].unsigned_value();
  r.aging_interval_us = *f[5].unsigned_value(); r.aging_rank_ceiling = *f[6].unsigned_value();
  r.per_target_max_concurrent = *f[7].unsigned_value();
  r.foreground_reserved_worker_slots = *f[8].unsigned_value();
  r.maintenance_reserved_worker_slots = *f[9].unsigned_value();
  r.emergency_override_allowed = f[10].bytes[0] != 0;
  std::size_t at = 11;
  if (f[at].id == 12) r.emergency_evidence_policy_uuid = f[at++].identity()->value;
  for (auto& limit : r.family_max_concurrent) limit = *f[at++].unsigned_value();
  for (auto& rank : r.base_priority_rank) rank = *f[at++].unsigned_value();
  for (auto& wait : r.queue_waits) {
    wait.maximum_queue_wait_us = *f[at++].unsigned_value();
    wait.allow_indefinite_deferral = f[at++].bytes[0] != 0;
  }
  if (!ValidCatalogSchedulerFairnessProfile(r)) return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,r};
}
inline bool IsCatalogSchedulerFairnessProfilePayload(std::string_view bytes) noexcept {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16) == 786435;
}
inline bool CatalogSchedulerFairnessProfileMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::policy || r.header.object_uuid.kind != UuidKind::object ||
      !uuid::IsEngineIdentityUuid(r.header.object_uuid.value)) return false;
  const auto decoded = DecodeCatalogSchedulerFairnessProfile(r.payload);
  return decoded.ok() && decoded.record->profile_uuid == r.header.object_uuid.value;
}
inline bool CatalogSchedulerFairnessProfileMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogSchedulerFairnessProfileMatchesHeader(BorrowCatalogTypedRecord(r));
}
inline bool CatalogSchedulerFairnessProfileMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto identity = [](const TypedUuid& id, UuidKind kind) {
    return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
  };
  if (!CatalogSchedulerFairnessProfileMatchesHeader(m.record) || m.object_subtype != "scheduler_fairness" ||
      m.authority_scope != CatalogAuthorityScope::local ||
      !identity(m.owning_schema_uuid,UuidKind::schema) ||
      !identity(m.record.header.parent_uuid,UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !identity(m.default_name_uuid,UuidKind::object) || !identity(m.name_vector_uuid,UuidKind::object) ||
      !identity(m.creator_transaction_uuid,UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogSchedulerFairnessProfile(m.record.payload);
  const auto& r = *decoded.record;
  return r.generation == m.definition_version &&
      (m.definition_version != 1 || (r.origin_transaction_uuid.value == m.creator_transaction_uuid.value &&
                                   r.origin_local_transaction_id == m.creator_local_transaction_id));
}
inline bool CatalogSchedulerFairnessProfileMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogSchedulerFairnessProfileMatchesMetadata(BorrowCatalogMetadataVersion(m));
}
inline bool CatalogSchedulerFairnessProfilePreservesOrigin(
    const CatalogMetadataVersionView& previous, const CatalogMetadataVersionView& successor) {
  const auto family = [](const auto& m) {
    return m.object_subtype == "scheduler_fairness" || IsCatalogSchedulerFairnessProfilePayload(m.record.payload);
  };
  if (!family(previous) && !family(successor)) return true;
  if (!CatalogSchedulerFairnessProfileMatchesMetadata(previous) ||
      !CatalogSchedulerFairnessProfileMatchesMetadata(successor)) return false;
  const auto a = DecodeCatalogSchedulerFairnessProfile(previous.record.payload);
  const auto b = DecodeCatalogSchedulerFairnessProfile(successor.record.payload);
  return a.record->profile_uuid == b.record->profile_uuid && a.record->database_uuid == b.record->database_uuid &&
      a.record->origin_transaction_uuid.value == b.record->origin_transaction_uuid.value &&
      a.record->origin_local_transaction_id == b.record->origin_local_transaction_id;
}
inline bool CatalogSchedulerFairnessProfilePreservesOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor) {
  return CatalogSchedulerFairnessProfilePreservesOrigin(
      BorrowCatalogMetadataVersion(previous),BorrowCatalogMetadataVersion(successor));
}

}  // namespace scratchbird::core::catalog
