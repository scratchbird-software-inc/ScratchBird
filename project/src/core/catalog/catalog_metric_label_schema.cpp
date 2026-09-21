// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_label_schema.hpp"
#include "metric_label_key.hpp"
#include "uuid.hpp"
#include <utility>

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
bool Valid(const CatalogMetricLabelSchema& r) {
  if (!uuid::IsEngineIdentityUuid(r.label_schema_uuid) || !r.generation ||
      !Identity(r.origin_transaction_uuid, UuidKind::transaction) ||
      !r.origin_local_transaction_id || r.labels.size() > 1024) return false;
  std::size_t key_bytes = 4;
  for (std::size_t i = 0; i < r.labels.size(); ++i) {
    const auto& label = r.labels[i];
    if (label.key.empty() || label.key.size() > 4096 ||
        label.key.find('\0') != label.key.npos || !LabelCode(label.value_type) ||
        !metrics::MetricScalarValid(metrics::MetricScalar(label.key))) return false;
    key_bytes += 4 + label.key.size();
    if (key_bytes > 65536) return false;
    for (std::size_t j = 0; j < i; ++j) if (label.key == r.labels[j].key) return false;
  }
  return true;
}
bool Family(const CatalogMetadataVersion& m) {
  return m.record.header.kind == CatalogRecordKind::metric_label_schema ||
      m.object_subtype == "metric_label_schema" || IsCatalogMetricLabelSchemaPayload(m.record.payload);
}
}  // namespace

const CatalogValueSchema& CatalogMetricLabelSchemaSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65545, 1, {
      {1,T::engine_identity,true,16,UuidKind::object}, {2,T::unsigned_integer,true,8},
      {3,T::utf8_text_list,true,65536}, {4,T::opaque_bytes,true,1024},
      {5,T::opaque_bytes,true,1024}, {6,T::boolean,true,1},
      {7,T::engine_identity,true,16,UuidKind::transaction}, {8,T::unsigned_integer,true,8}
  }};
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
CatalogMetricLabelSchemaResult DecodeCatalogMetricLabelSchema(std::string_view bytes) {
  if (bytes.size() > 67721) return {CatalogValueError::size_limit,{}};
  const auto result = DecodeCatalogValueBlock(CatalogMetricLabelSchemaSchema(),
      std::vector<byte>(bytes.begin(),bytes.end()));
  if (!result.ok()) return {result.error,{}};
  const auto& f = result.fields;
  const auto& keys = std::get<std::vector<std::string>>(f[2].value);
  const auto& flags = std::get<std::vector<byte>>(f[3].value);
  const auto& types = std::get<std::vector<byte>>(f[4].value);
  if (keys.size() > 1024 || flags.size() != keys.size() || types.size() != keys.size()) return {};
  CatalogMetricLabelSchema r;
  r.label_schema_uuid = std::get<TypedUuid>(f[0].value).value;
  r.generation = std::get<u64>(f[1].value);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (flags[i] & ~3u) return {};
    metrics::MetricLabelType type;
    switch (types[i]) {
      case 1: type = metrics::MetricLabelType::text; break;
      case 2: type = metrics::MetricLabelType::system_uuid; break;
      case 3: type = metrics::MetricLabelType::uuid_value; break;
      default: return {};
    }
    r.labels.push_back({keys[i],bool(flags[i]&1),bool(flags[i]&2),type});
  }
  r.cluster_only = std::get<bool>(f[5].value);
  r.origin_transaction_uuid = std::get<TypedUuid>(f[6].value);
  r.origin_local_transaction_id = std::get<u64>(f[7].value);
  if (!Valid(r)) return {};
  return {CatalogValueError::none,std::move(r)};
}
bool IsCatalogMetricLabelSchemaPayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16) == 65545;
}
bool CatalogMetricLabelSchemaMatchesHeader(const CatalogTypedRecord& r) {
  if (r.header.kind != CatalogRecordKind::metric_label_schema ||
      !Identity(r.header.object_uuid,UuidKind::object)) return false;
  const auto decoded = DecodeCatalogMetricLabelSchema(r.payload);
  return decoded.ok() && decoded.record->label_schema_uuid == r.header.object_uuid.value;
}
bool CatalogMetricLabelSchemaMatchesMetadata(const CatalogMetadataVersion& m) {
  if (!CatalogMetricLabelSchemaMatchesHeader(m.record) || m.object_subtype != "metric_label_schema" ||
      !Identity(m.owning_schema_uuid,UuidKind::schema) || !Identity(m.record.header.parent_uuid,UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !Identity(m.default_name_uuid,UuidKind::object) || !Identity(m.name_vector_uuid,UuidKind::object) ||
      !Identity(m.security_policy_uuid,UuidKind::object) || !Identity(m.creator_transaction_uuid,UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogMetricLabelSchema(m.record.payload);
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
  const auto a = DecodeCatalogMetricLabelSchema(previous.record.payload), b = DecodeCatalogMetricLabelSchema(successor.record.payload);
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
