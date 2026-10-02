// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_label_schema.hpp"
#include "metric_label_key.hpp"
#include "uuid.hpp"
#include <utility>
#include <array>

namespace scratchbird::core::catalog {
namespace {
bool Identity(const TypedUuid& value, UuidKind kind) {
  return value.kind == kind && uuid::IsEngineIdentityUuid(value.value);
}
u8 LabelCode(metrics::MetricLabelType type) {
  switch (type) {
    case metrics::MetricLabelType::text: return 1;
    case metrics::MetricLabelType::system_uuid: return 2;
    case metrics::MetricLabelType::uuid_value: return 3;
  }
  return 0;
}
template<class R, class TextValid> bool Valid(const R& r, TextValid text_valid) {
  if (!uuid::IsEngineIdentityUuid(r.label_schema_uuid) || !r.generation ||
      !Identity(r.origin_transaction_uuid, UuidKind::transaction) ||
      !r.origin_local_transaction_id || r.labels.size() > 1024) return false;
  std::size_t key_bytes = 4;
  for (auto it = r.labels.begin(); it != r.labels.end(); ++it) {
    const auto& label = *it;
    if (label.key.empty() || label.key.size() > 4096 ||
        label.key.find('\0') != label.key.npos || !LabelCode(label.value_type) ||
        !text_valid(label.key)) return false;
    key_bytes += 4 + label.key.size();
    if (key_bytes > 65536) return false;
    for (auto prior = r.labels.begin(); prior != it; ++prior)
      if (label.key == (*prior).key) return false;
  }
  return true;
}
bool Valid(const CatalogMetricLabelSchema& r) {
  return Valid(r, [](const std::string& key) {
    return metrics::MetricScalarValid(metrics::MetricScalar(key));
  });
}
using T = CatalogValueType;
constexpr std::array<CatalogValueFieldSchema,8> kFields{{
    {1,T::engine_identity,true,16,UuidKind::object}, {2,T::unsigned_integer,true,8},
    {3,T::utf8_text_list,true,65536}, {4,T::opaque_bytes,true,1024},
    {5,T::opaque_bytes,true,1024}, {6,T::boolean,true,1},
    {7,T::engine_identity,true,16,UuidKind::transaction}, {8,T::unsigned_integer,true,8}
}};
bool Family(const CatalogMetadataVersion& m) {
  return m.record.header.kind == CatalogRecordKind::metric_label_schema ||
      m.object_subtype == "metric_label_schema" || IsCatalogMetricLabelSchemaPayload(m.record.payload);
}
}  // namespace

const CatalogValueSchema& CatalogMetricLabelSchemaSchema() {
  static const CatalogValueSchema schema{65545, 1, {kFields.begin(),kFields.end()}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogMetricLabelSchema(const CatalogMetricLabelSchema& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value,{}};
  std::vector<std::string> keys;
  std::vector<byte> flags, types;
  for (const auto& label : r.labels) {
    keys.push_back(label.key);
    flags.push_back(static_cast<byte>(label.required | (label.sensitive << 1)));
    types.push_back(LabelCode(label.value_type));
  }
  std::vector<CatalogValueField> fields;
  fields.reserve(8);
  fields.push_back({1,TypedUuid{UuidKind::object,r.label_schema_uuid}});
  fields.push_back({2,r.generation});
  fields.push_back({3,std::move(keys)});
  fields.push_back({4,std::move(flags)});
  fields.push_back({5,std::move(types)});
  fields.push_back({6,r.cluster_only});
  fields.push_back({7,r.origin_transaction_uuid});
  fields.push_back({8,r.origin_local_transaction_id});
  return EncodeCatalogValueBlock(CatalogMetricLabelSchemaSchema(),fields);
}
CatalogMetricLabelSequenceView::Iterator::value_type
CatalogMetricLabelSequenceView::Iterator::operator*() const {
  const auto length = platform::LoadLittle32(key_);
  const auto type = *types_ == 1 ? metrics::MetricLabelType::text :
      *types_ == 2 ? metrics::MetricLabelType::system_uuid : metrics::MetricLabelType::uuid_value;
  return {{reinterpret_cast<const char*>(key_ + 4),length},
      bool(*flags_ & 1),bool(*flags_ & 2),type};
}
CatalogMetricLabelSequenceView::Iterator& CatalogMetricLabelSequenceView::Iterator::operator++() {
  key_ += 4 + platform::LoadLittle32(key_); ++flags_; ++types_; return *this;
}
CatalogMetricLabelSchemaViewResult DecodeCatalogMetricLabelSchemaView(std::string_view bytes) {
  if (bytes.size() > 67721) return {CatalogValueError::size_limit,{}};
  std::array<CatalogValueFieldView,8> backing;
  const auto result = DecodeCatalogValueBlockInto({65545,1,kFields},
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if (!result.ok()) return {result.error,{}};
  const auto& f = result.fields;
  const auto keys = f[2].bytes, flags = f[3].bytes, types = f[4].bytes;
  const auto count = platform::LoadLittle32(keys.data());
  if (count > 1024 || flags.size() != count || types.size() != count) return {};
  for (std::size_t i = 0; i < count; ++i)
    if ((flags[i] & ~3u) || types[i] < 1 || types[i] > 3) return {};
  CatalogMetricLabelSchemaView r;
  r.label_schema_uuid = f[0].identity()->value;
  r.generation = *f[1].unsigned_value();
  r.labels = CatalogMetricLabelSequenceView(keys.subspan(4),flags,types);
  r.cluster_only = f[5].bytes[0] != 0;
  r.origin_transaction_uuid = *f[6].identity();
  r.origin_local_transaction_id = *f[7].unsigned_value();
  // The common value validator has already validated every key's strict UTF8.
  if (!Valid(r, [](std::string_view) { return true; })) return {};
  return {CatalogValueError::none,std::move(r)};
}
CatalogMetricLabelSchemaResult DecodeCatalogMetricLabelSchema(std::string_view bytes) {
  const auto decoded = DecodeCatalogMetricLabelSchemaView(bytes);
  if (!decoded.ok()) return {decoded.error,{}};
  const auto& v = *decoded.record;
  CatalogMetricLabelSchema r{v.label_schema_uuid,v.generation,{},v.cluster_only,
      v.origin_transaction_uuid,v.origin_local_transaction_id};
  r.labels.reserve(v.labels.size());
  for (const auto label : v.labels)
    r.labels.push_back({std::string(label.key),label.required,label.sensitive,label.value_type});
  return {CatalogValueError::none,std::move(r)};
}
bool IsCatalogMetricLabelSchemaPayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16) == 65545;
}
bool CatalogMetricLabelSchemaMatchesHeader(const CatalogTypedRecord& r) {
  if (r.header.kind != CatalogRecordKind::metric_label_schema ||
      !Identity(r.header.object_uuid,UuidKind::object)) return false;
  const auto decoded = DecodeCatalogMetricLabelSchemaView(r.payload);
  return decoded.ok() && decoded.record->label_schema_uuid == r.header.object_uuid.value;
}
bool CatalogMetricLabelSchemaMatchesMetadata(const CatalogMetadataVersion& m) {
  if (!CatalogMetricLabelSchemaMatchesHeader(m.record) || m.object_subtype != "metric_label_schema" ||
      !Identity(m.owning_schema_uuid,UuidKind::schema) || !Identity(m.record.header.parent_uuid,UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !Identity(m.default_name_uuid,UuidKind::object) || !Identity(m.name_vector_uuid,UuidKind::object) ||
      !Identity(m.security_policy_uuid,UuidKind::object) || !Identity(m.creator_transaction_uuid,UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogMetricLabelSchemaView(m.record.payload);
  const auto& r = *decoded.record;
  return r.generation == m.definition_version &&
      m.authority_scope == (r.cluster_only ? CatalogAuthorityScope::cluster : CatalogAuthorityScope::local) &&
      r.origin_local_transaction_id <= m.creator_local_transaction_id &&
      (m.definition_version != 1 || (r.origin_transaction_uuid.value == m.creator_transaction_uuid.value &&
                                   r.origin_local_transaction_id == m.creator_local_transaction_id));
}
bool CatalogMetricLabelSchemaPreservesOrigin(const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor) {
  if (!Family(previous) && !Family(successor)) return true;
  if (!CatalogMetricLabelSchemaMatchesMetadata(previous) || !CatalogMetricLabelSchemaMatchesMetadata(successor)) return false;
  const auto a = DecodeCatalogMetricLabelSchemaView(previous.record.payload), b = DecodeCatalogMetricLabelSchemaView(successor.record.payload);
  return a.record->label_schema_uuid == b.record->label_schema_uuid && a.record->cluster_only == b.record->cluster_only &&
      a.record->origin_transaction_uuid.value == b.record->origin_transaction_uuid.value &&
      a.record->origin_local_transaction_id == b.record->origin_local_transaction_id;
}
bool CatalogMetricLabelSchemaMatchesDescriptor(const CatalogMetricLabelSchema& schema,
    const metrics::MetricDescriptorDefinition& d, const metrics::MetricDescriptorBinding& b) {
  if (!Valid(schema) || !metrics::MetricDescriptorReferencesValid(d,b) ||
      b.label_schema_uuid != schema.label_schema_uuid || b.label_schema_generation != schema.generation ||
      d.cluster_only != schema.cluster_only || d.labels.size() != schema.labels.size()) return false;
  for (std::size_t i = 0; i < d.labels.size(); ++i) {
    const auto& a = d.labels[i]; const auto& s = schema.labels[i];
    if (a.key != s.key || a.required != s.required || a.sensitive != s.sensitive || a.value_type != s.value_type) return false;
  }
  return true;
}
}  // namespace scratchbird::core::catalog
