// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_object_lifecycle.hpp"
#include "behavior_support/api_behavior_record_codec.hpp"
#include <tuple>
#include <type_traits>
#include <variant>

namespace scratchbird::engine::internal_api {
namespace catalog_record_codec {
// Each declared field has one fixed binary representation. UUIDs are raw16;
// strings are length framed and cannot delimit or inject another field.
inline bool Put(std::string& out, const EngineUuid& value) {
  if (!value.is_nil() && !core::uuid::IsEngineIdentityUuid(value)) return false;
  out.append(reinterpret_cast<const char*>(value.bytes.data()), 16);
  return true;
}
inline bool Put(std::string& out, const std::string& value) {
  return value.size() <= kApiBehaviorRecordMaximumBytes &&
         out.size() <= kApiBehaviorRecordMaximumBytes - value.size() &&
         AppendBinaryString(&out, value);
}
inline bool Put(std::string& out, std::uint64_t value) { AppendBinaryU64(&out, value); return true; }
inline bool Put(std::string& out, std::uint32_t value) { AppendBinaryU32(&out, value); return true; }
inline bool Put(std::string& out, bool value) { AppendBinaryU8(&out, value ? 1 : 0); return true; }
inline bool Get(std::span<const std::uint8_t> in, std::size_t& cursor, EngineUuid& value) {
  if (cursor > in.size() || in.size() - cursor < 16) return false;
  std::copy_n(in.begin() + cursor, 16, value.bytes.begin()); cursor += 16;
  return value.is_nil() || core::uuid::IsEngineIdentityUuid(value);
}
inline bool Get(std::span<const std::uint8_t> in, std::size_t& cursor, std::string& value) {
  return ReadBinaryString(in, &cursor, &value);
}
inline bool Get(std::span<const std::uint8_t> in, std::size_t& cursor, std::uint64_t& value) { return ReadBinaryU64(in, &cursor, &value); }
inline bool Get(std::span<const std::uint8_t> in, std::size_t& cursor, std::uint32_t& value) { return ReadBinaryU32(in, &cursor, &value); }
inline bool Get(std::span<const std::uint8_t> in, std::size_t& cursor, bool& value) {
  std::uint8_t byte = 0;
  if (!ReadBinaryU8(in, &cursor, &byte) || byte > 1) return false;
  value = byte != 0; return true;
}
template<class Record> struct Traits;
template<> struct Traits<EngineCatalogObjectRecord> {
  static constexpr const char* kind = "EngineCatalogObjectRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.object_uuid, r.object_kind, r.schema_uuid, r.owner_principal_uuid, r.lifecycle_state, r.definition_epoch, r.metadata_epoch, r.payload, r.synonym_target_uuid, r.synonym_target_class, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogObjectRecord& r) { return r.object_uuid; }
};
template<> struct Traits<EngineCatalogNameRecord> {
  static constexpr const char* kind = "EngineCatalogNameRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.name_entry_uuid, r.object_uuid, r.object_kind, r.schema_uuid, r.language_tag, r.name_class, r.identifier_profile_uuid, r.raw_name_text, r.display_name, r.normalized_lookup_key, r.exact_lookup_key, r.requires_exact_match, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogNameRecord& r) { return r.name_entry_uuid; }
};
template<> struct Traits<EngineCatalogDependencyRecord> {
  static constexpr const char* kind = "EngineCatalogDependencyRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.source_uuid, r.source_kind, r.dependency_uuid, r.dependency_kind, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogDependencyRecord& r) { return r.source_uuid; }
};
template<> struct Traits<EngineCatalogColumnMetadataRecord> {
  static constexpr const char* kind = "EngineCatalogColumnMetadataRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.column_uuid, r.owner_object_uuid, r.descriptor_kind, r.canonical_type_name, r.default_expression_envelope, r.ordinal, r.nullable, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogColumnMetadataRecord& r) { return r.column_uuid; }
};
template<> struct Traits<EngineCatalogConstraintDescriptorRecord> {
  static constexpr const char* kind = "EngineCatalogConstraintDescriptorRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.constraint_uuid, r.constraint_class, r.owner_object_uuid, r.name_ref_uuid, r.constraint_policy_version_uuid, r.enforcement_timing, r.validation_state, r.trust_state, r.support_requirement, r.predicate_sblr_uuid, r.diagnostic_profile_uuid, r.metrics_profile_uuid, r.conformance_profile_uuid, r.constraint_hash, r.canonical_constraint_envelope, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogConstraintDescriptorRecord& r) { return r.constraint_uuid; }
};
template<> struct Traits<EngineCatalogKeyDescriptorRecord> {
  static constexpr const char* kind = "EngineCatalogKeyDescriptorRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.key_descriptor_uuid, r.constraint_uuid, r.key_class, r.owner_object_uuid, r.component_order_hash, r.comparison_profile_hash, r.null_policy, r.canonical_encoding_uuid, r.candidate_reference_allowed, r.key_state, r.key_hash, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogKeyDescriptorRecord& r) { return r.key_descriptor_uuid; }
};
template<> struct Traits<EngineCatalogConstraintSubjectRecord> {
  static constexpr const char* kind = "EngineCatalogConstraintSubjectRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.subject_uuid, r.constraint_uuid, r.subject_kind, r.subject_object_uuid, r.subject_descriptor, r.expression_sblr_uuid, r.ordinal, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogConstraintSubjectRecord& r) { return r.subject_uuid; }
};
template<> struct Traits<EngineCatalogConstraintDependencyRecord> {
  static constexpr const char* kind = "EngineCatalogConstraintDependencyRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.dependency_uuid, r.constraint_uuid, r.dependency_kind, r.dependency_object_uuid, r.dependency_version_uuid, r.invalidation_action, r.dependency_hash, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogConstraintDependencyRecord& r) { return r.dependency_uuid; }
};
template<> struct Traits<EngineCatalogConstraintSupportStructureRecord> {
  static constexpr const char* kind = "EngineCatalogConstraintSupportStructureRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.support_binding_uuid, r.constraint_uuid, r.support_uuid, r.support_class, r.support_family, r.coverage_scope_hash, r.durability_class, r.residency_class, r.validity_state, r.enforcement_role, r.binding_hash, r.metadata_epoch, r.deleted);
  }
  static const EngineUuid& Identity(const EngineCatalogConstraintSupportStructureRecord& r) { return r.support_binding_uuid; }
};
template<> struct Traits<EngineCatalogRetireNamesRecord> {
  static constexpr const char* kind = "EngineCatalogRetireNamesRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.object_uuid, r.metadata_epoch);
  }
  static const EngineUuid& Identity(const EngineCatalogRetireNamesRecord& r) { return r.object_uuid; }
};
template<> struct Traits<EngineCatalogCacheInvalidationRecord> {
  static constexpr const char* kind = "EngineCatalogCacheInvalidationRecord";
  template<class T> static auto Fields(T& r) {
    return std::tie(r.creator_tx, r.object_uuid, r.operation_id, r.metadata_epoch, r.name_resolution_epoch, r.resource_epoch);
  }
  static const EngineUuid& Identity(const EngineCatalogCacheInvalidationRecord& r) { return r.object_uuid; }
};
}  // namespace catalog_record_codec
using CatalogLifecycleRecord = std::variant<EngineCatalogObjectRecord,
    EngineCatalogNameRecord,
    EngineCatalogDependencyRecord,
    EngineCatalogColumnMetadataRecord,
    EngineCatalogConstraintDescriptorRecord,
    EngineCatalogKeyDescriptorRecord,
    EngineCatalogConstraintSubjectRecord,
    EngineCatalogConstraintDependencyRecord,
    EngineCatalogConstraintSupportStructureRecord,
    EngineCatalogRetireNamesRecord,
    EngineCatalogCacheInvalidationRecord>;
template<class Record>
bool EncodeCatalogLifecycleRecord(const Record& record, std::string* output) {
  using Traits = catalog_record_codec::Traits<Record>;
  ApiBehaviorRecord frame;
  frame.object_uuid = Traits::Identity(record);
  frame.creator_tx = record.creator_tx;
  frame.operation_id = "catalog.lifecycle.record";
  frame.object_kind = Traits::kind;
  frame.state = "record";
  frame.payload = kCatalogObjectLifecycleEventMagic;
  if (!std::apply([&](const auto&... field) {
        return (catalog_record_codec::Put(frame.payload, field) && ...);
      }, Traits::Fields(record))) return false;
  return EncodeApiBehaviorRecord(frame, output);
}
template<class Record>
bool DecodeCatalogLifecycleBody(const ApiBehaviorRecord& frame, Record* output) {
  using Traits = catalog_record_codec::Traits<Record>;
  if (!output || frame.object_kind != Traits::kind || frame.operation_id != "catalog.lifecycle.record" ||
      frame.state != "record" || frame.deleted || !frame.default_name.empty() ||
      !frame.target_database_uuid.is_nil() || !frame.target_schema_uuid.is_nil() || !frame.target_object_uuid.is_nil() ||
      frame.payload.size() < 8 || frame.payload.compare(0, 8, kCatalogObjectLifecycleEventMagic) != 0) return false;
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(frame.payload.data()), frame.payload.size());
  std::size_t cursor = 8; Record candidate;
  if (!std::apply([&](auto&... field) {
        return (catalog_record_codec::Get(bytes, cursor, field) && ...);
      }, Traits::Fields(candidate)) || cursor != bytes.size() ||
      candidate.creator_tx != frame.creator_tx || Traits::Identity(candidate) != frame.object_uuid) return false;
  *output = std::move(candidate); return true;
}
inline bool DecodeCatalogLifecycleFrame(const ApiBehaviorRecord& frame, CatalogLifecycleRecord* output) {
  if (!output) return false;
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogObjectRecord>::kind) {
    EngineCatalogObjectRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogNameRecord>::kind) {
    EngineCatalogNameRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogDependencyRecord>::kind) {
    EngineCatalogDependencyRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogColumnMetadataRecord>::kind) {
    EngineCatalogColumnMetadataRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogConstraintDescriptorRecord>::kind) {
    EngineCatalogConstraintDescriptorRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogKeyDescriptorRecord>::kind) {
    EngineCatalogKeyDescriptorRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogConstraintSubjectRecord>::kind) {
    EngineCatalogConstraintSubjectRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogConstraintDependencyRecord>::kind) {
    EngineCatalogConstraintDependencyRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogConstraintSupportStructureRecord>::kind) {
    EngineCatalogConstraintSupportStructureRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogRetireNamesRecord>::kind) {
    EngineCatalogRetireNamesRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  if (frame.object_kind == catalog_record_codec::Traits<EngineCatalogCacheInvalidationRecord>::kind) {
    EngineCatalogCacheInvalidationRecord record;
    if (!DecodeCatalogLifecycleBody(frame, &record)) return false;
    *output = std::move(record); return true;
  }
  return false;
}
inline bool ReadCatalogLifecycleRecord(std::istream& input, CatalogLifecycleRecord* output) {
  ApiBehaviorRecord frame;
  return ReadApiBehaviorRecord(input, &frame) && DecodeCatalogLifecycleFrame(frame, output);
}
}  // namespace scratchbird::engine::internal_api
