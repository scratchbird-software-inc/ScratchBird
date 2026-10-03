// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_visibility_policy.hpp"
#include "uuid.hpp"
#include <array>
#include <algorithm>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
bool Identity(const TypedUuid& id, UuidKind kind) {
  return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
}
bool Right(std::string_view right, bool optional) {
  if (right.empty()) return optional;
  return right.size() <= 128 && right.front() >= 'A' && right.front() <= 'Z' &&
      std::all_of(right.begin(),right.end(),[](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      });
}
template <typename Record> bool Valid(const Record& r) {
  return uuid::IsEngineIdentityUuid(r.policy_uuid) && r.generation &&
      uuid::IsEngineIdentityUuid(r.database_uuid) && uuid::IsEngineIdentityUuid(r.metric_uuid) &&
      r.policy_uuid != r.metric_uuid && Right(r.read_right,false) && Right(r.sensitive_read_right,true) &&
      Identity(r.origin_transaction_uuid,UuidKind::transaction) && r.origin_local_transaction_id;
}
bool Family(const CatalogMetadataVersionView& m) {
  return m.object_subtype == "metric_visibility" || IsCatalogMetricVisibilityPolicyPayload(m.record.payload);
}
}
constexpr CatalogValueSchemaView CatalogMetricVisibilityPolicySchemaView() {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
      {1,T::engine_identity,true,16,UuidKind::object}, {2,T::unsigned_integer,true,8},
      {3,T::engine_identity,true,16,UuidKind::database}, {4,T::engine_identity,true,16,UuidKind::object},
      {5,T::utf8_text,true,128}, {6,T::utf8_text,true,128},
      {7,T::engine_identity,true,16,UuidKind::transaction}, {8,T::unsigned_integer,true,8}};
  return {65559, 1, fields};
}
const CatalogValueSchema& CatalogMetricVisibilityPolicySchema() {
  static const CatalogValueSchema schema = [] {
    const auto view = CatalogMetricVisibilityPolicySchemaView();
    return CatalogValueSchema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  }();
  return schema;
}
CatalogValueEncodeResult EncodeCatalogMetricVisibilityPolicy(const CatalogMetricVisibilityPolicy& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value,{}};
  return EncodeCatalogValueBlock(CatalogMetricVisibilityPolicySchema(),{
      {1,TypedUuid{UuidKind::object,r.policy_uuid}}, {2,r.generation},
      {3,TypedUuid{UuidKind::database,r.database_uuid}}, {4,TypedUuid{UuidKind::object,r.metric_uuid}},
      {5,r.read_right}, {6,r.sensitive_read_right}, {7,r.origin_transaction_uuid},
      {8,r.origin_local_transaction_id}});
}
CatalogMetricVisibilityPolicyViewResult DecodeCatalogMetricVisibilityPolicyView(std::string_view bytes) {
  if (bytes.size()>1024) return {CatalogValueError::size_limit,{}};
  std::array<CatalogValueFieldView, 8> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogMetricVisibilityPolicySchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error,{}};
  const auto& f=decoded.fields;
  CatalogMetricVisibilityPolicyView r;
  r.policy_uuid=f[0].identity()->value;
  r.generation=*f[1].unsigned_value();
  r.database_uuid=f[2].identity()->value;
  r.metric_uuid=f[3].identity()->value;
  r.read_right=std::string_view(reinterpret_cast<const char*>(f[4].bytes.data()), f[4].bytes.size());
  r.sensitive_read_right=std::string_view(reinterpret_cast<const char*>(f[5].bytes.data()), f[5].bytes.size());
  r.origin_transaction_uuid=*f[6].identity();
  r.origin_local_transaction_id=*f[7].unsigned_value();
  if (!Valid(r)) return {};
  return {CatalogValueError::none,std::move(r)};
}
CatalogMetricVisibilityPolicyResult DecodeCatalogMetricVisibilityPolicy(std::string_view bytes) {
  const auto decoded = DecodeCatalogMetricVisibilityPolicyView(bytes);
  if (!decoded.ok()) return {decoded.error, {}};
  const auto& v = *decoded.record;
  return {CatalogValueError::none, CatalogMetricVisibilityPolicy{
      v.policy_uuid, v.generation, v.database_uuid, v.metric_uuid,
      std::string(v.read_right), std::string(v.sensitive_read_right),
      v.origin_transaction_uuid, v.origin_local_transaction_id}};
}
bool IsCatalogMetricVisibilityPolicyPayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4)=="SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==65559;
}
bool CatalogMetricVisibilityPolicyMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind!=CatalogRecordKind::policy || !Identity(r.header.object_uuid,UuidKind::object)) return false;
  const auto policy=DecodeCatalogMetricVisibilityPolicyView(r.payload);
  return policy.ok() && policy.record->policy_uuid==r.header.object_uuid.value;
}
bool CatalogMetricVisibilityPolicyMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogMetricVisibilityPolicyMatchesHeader(BorrowCatalogTypedRecord(r));
}
bool CatalogMetricVisibilityPolicyMatchesMetadata(const CatalogMetadataVersionView& m) {
  if (!CatalogMetricVisibilityPolicyMatchesHeader(m.record) || m.object_subtype!="metric_visibility" ||
      m.authority_scope!=CatalogAuthorityScope::local || !Identity(m.owning_schema_uuid,UuidKind::schema) ||
      !Identity(m.record.header.parent_uuid,UuidKind::object) ||
      m.owning_schema_uuid.value!=m.record.header.parent_uuid.value ||
      !Identity(m.default_name_uuid,UuidKind::object) || !Identity(m.name_vector_uuid,UuidKind::object) ||
      !Identity(m.creator_transaction_uuid,UuidKind::transaction)) return false;
  const auto policy=DecodeCatalogMetricVisibilityPolicyView(m.record.payload);
  const auto& p=*policy.record;
  return p.generation==m.definition_version && p.origin_local_transaction_id<=m.creator_local_transaction_id &&
      (m.definition_version!=1 || (p.origin_transaction_uuid.value==m.creator_transaction_uuid.value &&
        p.origin_local_transaction_id==m.creator_local_transaction_id));
}
bool CatalogMetricVisibilityPolicyPreservesOrigin(
    const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) {
  if (!Family(a) && !Family(b)) return true;
  if (!CatalogMetricVisibilityPolicyMatchesMetadata(a) || !CatalogMetricVisibilityPolicyMatchesMetadata(b)) return false;
  const auto before=DecodeCatalogMetricVisibilityPolicyView(a.record.payload);
  const auto after=DecodeCatalogMetricVisibilityPolicyView(b.record.payload);
  return before.record->policy_uuid==after.record->policy_uuid &&
      before.record->database_uuid==after.record->database_uuid && before.record->metric_uuid==after.record->metric_uuid &&
      before.record->origin_transaction_uuid.value==after.record->origin_transaction_uuid.value &&
      before.record->origin_local_transaction_id==after.record->origin_local_transaction_id;
}
bool CatalogMetricVisibilityPolicyMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogMetricVisibilityPolicyMatchesMetadata(BorrowCatalogMetadataVersion(m));
}
bool CatalogMetricVisibilityPolicyPreservesOrigin(const CatalogMetadataVersion& a, const CatalogMetadataVersion& b) {
  return CatalogMetricVisibilityPolicyPreservesOrigin(BorrowCatalogMetadataVersion(a), BorrowCatalogMetadataVersion(b));
}
}  // namespace scratchbird::core::catalog
