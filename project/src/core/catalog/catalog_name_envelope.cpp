// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_name_envelope.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
using Error = CatalogNameEnvelopeError;
constexpr std::size_t kHeaderBytes = 176;
constexpr std::size_t kMaxBytes = 131072;
struct IdentityField {
  TypedUuid CatalogNameVersionBinding::* member;
  UuidKind kind;
  std::size_t offset;
};
constexpr std::array<IdentityField,6> kIdentityFields{{
  {&CatalogNameVersionBinding::database_uuid, UuidKind::database,24},
  {&CatalogNameVersionBinding::filespace_uuid, UuidKind::filespace,40},
  {&CatalogNameVersionBinding::row_uuid, UuidKind::row,56},
  {&CatalogNameVersionBinding::version_uuid, UuidKind::row,72},
  {&CatalogNameVersionBinding::catalog_object_uuid, UuidKind::object,88},
  {&CatalogNameVersionBinding::creating_transaction_uuid, UuidKind::transaction,104}
}};
bool SameIdentity(const TypedUuid& a,const TypedUuid& b) {
  return a.kind == b.kind && a.value == b.value;
}
Error Validate(const CatalogNameVersionBinding& binding) {
  for (const auto& field : kIdentityFields) {
    const auto& id = binding.*field.member;
    if (id.kind != field.kind || !uuid::IsEngineIdentityUuid(id.value))
      return Error::invalid_identity;
  }
  if (!binding.page_id || !binding.storage_generation || !binding.version_sequence ||
      !binding.creating_transaction_number || !binding.catalog_generation)
    return Error::invalid_header;
  return Error::none;
}
bool SameBinding(const CatalogNameVersionBinding& a,const CatalogNameVersionBinding& b) {
  for (const auto& field : kIdentityFields)
    if (!SameIdentity(a.*field.member,b.*field.member)) return false;
  return a.page_id == b.page_id && a.slot_id == b.slot_id &&
      a.storage_generation == b.storage_generation && a.version_sequence == b.version_sequence &&
      a.creating_transaction_number == b.creating_transaction_number &&
      a.catalog_generation == b.catalog_generation;
}
template<class Envelope> bool PayloadMatches(const Envelope& record) {
  using Payload = decltype(record.payload);
  const auto& b = record.binding;
  if (const auto* v = std::get_if<std::variant_alternative_t<0,Payload>>(&record.payload))
    return SameIdentity(b.catalog_object_uuid,v->name_vector_uuid) &&
        b.catalog_generation == v->catalog_generation_id;
  if (const auto* e = std::get_if<std::variant_alternative_t<1,Payload>>(&record.payload))
    return SameIdentity(b.catalog_object_uuid,e->name_entry_uuid) &&
        b.catalog_generation == e->catalog_generation_id &&
        (b.version_sequence != 1 || SameIdentity(b.creating_transaction_uuid,e->created_transaction_uuid));
  return false;
}
void Put(std::vector<byte>& out,std::size_t offset,u64 value,unsigned width) {
  for (unsigned i=0;i<width;++i) out[offset+i] = static_cast<byte>(value >> (i*8));
}
u64 Get(std::span<const byte> in,std::size_t offset,unsigned width) {
  u64 value = 0;
  for (unsigned i=0;i<width;++i) value |= static_cast<u64>(in[offset+i]) << (i*8);
  return value;
}
CatalogNameVersionBinding ReadBinding(std::span<const byte> bytes) {
  CatalogNameVersionBinding b;
  for (const auto& field : kIdentityFields) {
    auto& id = b.*field.member;
    id.kind = field.kind;
    std::copy_n(bytes.begin()+field.offset,16,id.value.bytes.begin());
  }
  b.page_id=Get(bytes,120,8); b.slot_id=static_cast<u32>(Get(bytes,128,4));
  b.storage_generation=Get(bytes,136,8); b.version_sequence=Get(bytes,144,8);
  b.creating_transaction_number=Get(bytes,152,8); b.catalog_generation=Get(bytes,160,8);
  return b;
}
}  // namespace
CatalogNamePayloadView BorrowCatalogNamePayload(const CatalogNamePayload& payload) {
  return std::visit([](const auto& record) -> CatalogNamePayloadView {
    if constexpr (std::is_same_v<std::decay_t<decltype(record)>, CatalogNameVector>)
      return BorrowCatalogNameVector(record);
    else return BorrowCatalogNameEntry(record);
  }, payload);
}
bool CatalogNamePayloadMatchesMetadata(const CatalogNamePayload& payload, const CatalogMetadataVersion& m) {
  if (payload.valueless_by_exception()) return false;
  return CatalogNamePayloadMatchesMetadata(BorrowCatalogNamePayload(payload), BorrowCatalogMetadataVersion(m));
}
bool CatalogNamePayloadMatchesMetadata(const CatalogNamePayloadView& payload, const CatalogMetadataVersionView& m) {
  if (m.record.header.kind != CatalogRecordKind::localized_name) return false;
  const auto optional_matches=[](const auto& value,const TypedUuid& common) {
    return value ? SameIdentity(*value,common) : common.kind == UuidKind::unknown && common.value.is_nil();
  };
  if (const auto* v=std::get_if<CatalogNameVectorView>(&payload)) {
    return ValidateCatalogNameVector(*v) == CatalogValueError::none && m.object_subtype == "name_vector" &&
        SameIdentity(v->name_vector_uuid,m.record.header.object_uuid) &&
        SameIdentity(v->object_uuid,m.record.header.parent_uuid) &&
        SameIdentity(v->name_vector_uuid,m.name_vector_uuid) &&
        SameIdentity(v->default_name_entry_uuid,m.default_name_uuid) &&
        optional_matches(v->owning_schema_uuid,m.owning_schema_uuid) &&
        SameIdentity(v->security_policy_uuid,m.security_policy_uuid) &&
        v->catalog_generation_id == m.catalog_generation &&
        v->name_vector_uuid.value != v->object_uuid.value &&
        v->name_vector_uuid.value != v->default_name_entry_uuid.value &&
        v->object_uuid.value != v->default_name_entry_uuid.value &&
        (!m.record.header.deleted || v->lifecycle_state == CatalogNameLifecycle::dropped);
  }
  if (const auto* e=std::get_if<CatalogNameEntryView>(&payload)) {
    return ValidateCatalogNameEntry(*e) == CatalogValueError::none && m.object_subtype == "name_entry" &&
        SameIdentity(e->name_entry_uuid,m.record.header.object_uuid) &&
        SameIdentity(e->name_vector_uuid,m.record.header.parent_uuid) &&
        SameIdentity(e->name_vector_uuid,m.name_vector_uuid) &&
        SameIdentity(e->name_entry_uuid,m.default_name_uuid) &&
        optional_matches(e->parent_schema_uuid,m.owning_schema_uuid) &&
        optional_matches(e->dropped_transaction_uuid,m.retired_transaction_uuid) &&
        SameIdentity(e->security_policy_uuid,m.security_policy_uuid) &&
        e->catalog_generation_id == m.catalog_generation && e->resource_epoch == m.resource_epoch &&
        (m.definition_version != 1 || SameIdentity(e->created_transaction_uuid,m.creator_transaction_uuid)) &&
        e->name_entry_uuid.value != e->name_vector_uuid.value &&
        e->name_entry_uuid.value != e->object_uuid.value &&
        e->name_vector_uuid.value != e->object_uuid.value &&
        (!m.record.header.deleted || e->lifecycle_state == CatalogNameLifecycle::dropped);
  }
  return false;
}
bool CatalogNamePayloadPreservesIdentity(const CatalogNamePayload& before,const CatalogNamePayload& after) {
  if (before.valueless_by_exception() || after.valueless_by_exception()) return false;
  return CatalogNamePayloadPreservesIdentity(BorrowCatalogNamePayload(before), BorrowCatalogNamePayload(after));
}
bool CatalogNamePayloadPreservesIdentity(const CatalogNamePayloadView& before,const CatalogNamePayloadView& after) {
  if (const auto* a=std::get_if<CatalogNameVectorView>(&before)) {
    const auto* b=std::get_if<CatalogNameVectorView>(&after);
    return b && SameIdentity(a->name_vector_uuid,b->name_vector_uuid) &&
        SameIdentity(a->object_uuid,b->object_uuid) && a->object_class == b->object_class;
  }
  if (const auto* a=std::get_if<CatalogNameEntryView>(&before)) {
    const auto* b=std::get_if<CatalogNameEntryView>(&after);
    return b && SameIdentity(a->name_entry_uuid,b->name_entry_uuid) &&
        SameIdentity(a->name_vector_uuid,b->name_vector_uuid) && SameIdentity(a->object_uuid,b->object_uuid) &&
        a->object_class == b->object_class && SameIdentity(a->created_transaction_uuid,b->created_transaction_uuid) &&
        b->name_resolution_epoch >= a->name_resolution_epoch;
  }
  return false;
}
CatalogNameEnvelopeEncodeResult EncodeCatalogNameEnvelope(const CatalogNameEnvelope& record) {
  const auto error = Validate(record.binding);
  if (error != Error::none) return {error,{}};
  if (!PayloadMatches(record)) return {Error::binding_mismatch,{}};
  CatalogValueEncodeResult payload;
  u32 schema_id = 0;
  if (const auto* v = std::get_if<CatalogNameVector>(&record.payload)) {
    payload=EncodeCatalogNameVector(*v); schema_id=327681;
  } else if (const auto* e = std::get_if<CatalogNameEntry>(&record.payload)) {
    payload=EncodeCatalogNameEntry(*e); schema_id=327682;
  } else return {Error::invalid_payload,{}};
  if (!payload.ok()) return {Error::invalid_payload,{}};
  if (payload.bytes.size() > kMaxBytes-kHeaderBytes) return {Error::size_limit,{}};

  CatalogNameEnvelopeEncodeResult result;
  auto& bytes=result.bytes;
  bytes.resize(kHeaderBytes+payload.bytes.size(),0);
  bytes[0]='S'; bytes[1]='B'; bytes[2]='C'; bytes[3]='R';
  Put(bytes,4,1,2); Put(bytes,6,kHeaderBytes,2); Put(bytes,8,bytes.size(),4);
  Put(bytes,12,payload.bytes.size(),4); Put(bytes,16,schema_id,4);
  Put(bytes,20,1,2); Put(bytes,22,5,2);
  const auto& b=record.binding;
  for (const auto& field:kIdentityFields) {
    const auto& id=b.*field.member;
    std::copy(id.value.bytes.begin(),id.value.bytes.end(),bytes.begin()+field.offset);
  }
  Put(bytes,120,b.page_id,8); Put(bytes,128,b.slot_id,4);
  Put(bytes,136,b.storage_generation,8); Put(bytes,144,b.version_sequence,8);
  Put(bytes,152,b.creating_transaction_number,8); Put(bytes,160,b.catalog_generation,8);
  std::copy(payload.bytes.begin(),payload.bytes.end(),bytes.begin()+kHeaderBytes);
  return result;
}
CatalogNameEnvelopeViewResult DecodeCatalogNameEnvelopeView(
    std::span<const byte> bytes,const CatalogNameVersionBinding& expected) {
  const auto expected_error=Validate(expected);
  if (expected_error!=Error::none) return {expected_error,{}};
  if (bytes.size()>kMaxBytes) return {Error::size_limit,{}};
  if (bytes.size()<kHeaderBytes || bytes[0]!='S' || bytes[1]!='B' || bytes[2]!='C' || bytes[3]!='R')
    return {Error::invalid_header,{}};
  if (Get(bytes,4,2)!=1) return {Error::unsupported_format,{}};
  if (Get(bytes,6,2)!=kHeaderBytes || Get(bytes,8,4)!=bytes.size() ||
      Get(bytes,12,4)!=bytes.size()-kHeaderBytes || Get(bytes,20,2)!=1 ||
      Get(bytes,22,2)!=5 || Get(bytes,132,4)!=0 || Get(bytes,168,8)!=0)
    return {Error::invalid_header,{}};
  const auto schema_id=Get(bytes,16,4);
  if (schema_id!=327681 && schema_id!=327682) return {Error::invalid_payload,{}};
  CatalogNameEnvelopeView record;
  record.binding=ReadBinding(bytes);
  const auto binding_error=Validate(record.binding);
  if (binding_error!=Error::none) return {binding_error,{}};
  if (!SameBinding(record.binding,expected)) return {Error::binding_mismatch,{}};
  const auto payload=bytes.subspan(kHeaderBytes);
  if (schema_id==327681) {
    auto decoded=DecodeCatalogNameVectorView(payload);
    if (!decoded.ok()) return {Error::invalid_payload,{}};
    record.payload=std::move(*decoded.record);
  } else {
    auto decoded=DecodeCatalogNameEntryView(payload);
    if (!decoded.ok()) return {Error::invalid_payload,{}};
    record.payload=std::move(*decoded.record);
  }
  if (!PayloadMatches(record)) return {Error::binding_mismatch,{}};
  return {Error::none,std::move(record)};
}
CatalogNameEnvelope MaterializeCatalogNameEnvelope(const CatalogNameEnvelopeView& record) {
  return {record.binding, std::visit([](const auto& payload) -> CatalogNamePayload {
    if constexpr(std::is_same_v<std::decay_t<decltype(payload)>,CatalogNameVectorView>)
      return MaterializeCatalogNameVector(payload);
    else return MaterializeCatalogNameEntry(payload);
  },record.payload)};
}
CatalogNameEnvelopeDecodeResult DecodeCatalogNameEnvelope(
    const std::vector<byte>& bytes,const CatalogNameVersionBinding& expected) {
  const auto decoded=DecodeCatalogNameEnvelopeView(bytes,expected);
  if(!decoded.ok())return {decoded.error,{}};
  return {Error::none,MaterializeCatalogNameEnvelope(*decoded.record)};
}
}  // namespace scratchbird::core::catalog
