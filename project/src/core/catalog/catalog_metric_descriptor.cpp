// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_descriptor.hpp"
#include "metric_label_key.hpp"
#include "metric_value_update.hpp"
#include "metric_descriptor_validation.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
using namespace metrics;
constexpr std::array kClasses{MetricType::counter, MetricType::gauge, MetricType::histogram,
    MetricType::rate, MetricType::state, MetricType::sample};
constexpr std::array kTypes{MetricScalarType::uint64, MetricScalarType::int64,
    MetricScalarType::float64, MetricScalarType::float128, MetricScalarType::decimal128,
    MetricScalarType::boolean, MetricScalarType::text, MetricScalarType::uuid, MetricScalarType::enumeration};
constexpr std::array kUnits{MetricUnit::bytes, MetricUnit::pages, MetricUnit::records,
    MetricUnit::transactions, MetricUnit::operations, MetricUnit::seconds, MetricUnit::milliseconds,
    MetricUnit::microseconds, MetricUnit::nanoseconds, MetricUnit::percent, MetricUnit::ratio,
    MetricUnit::revisions, MetricUnit::events, MetricUnit::errors, MetricUnit::conflicts, MetricUnit::none};
constexpr std::array kVisibility{MetricVisibilityScope::baseline, MetricVisibilityScope::self,
    MetricVisibilityScope::family, MetricVisibilityScope::all, MetricVisibilityScope::cluster};
constexpr std::array kLabels{MetricLabelType::text, MetricLabelType::system_uuid, MetricLabelType::uuid_value};
template<class T, std::size_t N> u64 Code(T value, const std::array<T, N>& values) {
  const auto it = std::find(values.begin(), values.end(), value);
  return it == values.end() ? 0 : static_cast<u64>(it - values.begin()) + 1;
}
template<class T, std::size_t N> bool FromCode(u64 code, const std::array<T, N>& values, T& value) {
  if (!code || code > N) return false;
  value = values[code - 1]; return true;
}
bool Identity(const TypedUuid& id, UuidKind kind) {
  return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
}
bool Text(std::string_view text, std::size_t max, bool allow_empty = false) {
  // The common value codec independently checks UTF-8 for every text field.
  return (allow_empty || !text.empty()) && text.size() <= max && text.find('\0') == text.npos;
}
std::size_t Width(MetricScalarType type) {
  switch (type) {
    case MetricScalarType::uint64: case MetricScalarType::int64: case MetricScalarType::float64: return 8;
    case MetricScalarType::float128: case MetricScalarType::decimal128: return 16;
    default: return 0;
  }
}
template<class R> bool Valid(const R& r) {
  const auto& d = r.definition;
  if (!MetricDescriptorReferencesValid(d, r.binding) ||
      !Identity(r.origin_transaction_uuid, UuidKind::transaction) || !r.origin_local_transaction_id ||
      !Code(d.type, kClasses) || !Code(d.unit, kUnits) || !Code(d.visibility, kVisibility) ||
      metrics::detail::ValidateScalarDescriptor(d) != MetricScalarError::none ||
      !metrics::detail::ValidateHistogramDescriptor(d) || d.histogram_buckets.size() > 4096 || d.enum_values.size() > 4096 ||
      ((d.type == MetricType::counter || d.type == MetricType::rate) && !Width(d.value_type)) ||
      (d.type == MetricType::state && d.value_type != MetricScalarType::enumeration) ||
      !Text(d.family, 4096) || !Text(d.namespace_path, 4096) || !Text(d.producer_owner, 4096) ||
      !Text(d.help, 16384, true) || !Text(d.security_family, 4096, true) ||
      d.labels.size() > 1024 || d.aliases.size() > 1024) return false;
  if (!MetricNamespaceMatchesScope(d)) return false;
  for (auto it=d.labels.begin();it!=d.labels.end();++it) {
    const auto& label=*it;
    if (!Text(label.key,4096) || !Code(label.value_type,kLabels)) return false;
    for (auto prior=d.labels.begin();prior!=it;++prior) if (label.key==(*prior).key) return false;
  }
  for (auto it=d.aliases.begin();it!=d.aliases.end();++it) {
    if (!Text(*it,4096)) return false;
    for (auto prior=d.aliases.begin();prior!=it;++prior) if (*it==*prior) return false;
  }
  return true;
}
std::vector<byte> NumericBytes(const MetricScalar& value) {
  std::vector<byte> bytes(Width(MetricScalarTypeOf(value)));
  if (const auto* v = std::get_if<u64>(&value)) platform::StoreLittle64(bytes.data(), *v);
  else if (const auto* v = std::get_if<std::int64_t>(&value)) platform::StoreLittle64(bytes.data(), std::bit_cast<u64>(*v));
  else if (const auto* v = std::get_if<double>(&value)) platform::StoreLittle64(bytes.data(), std::bit_cast<u64>(*v));
  else if (const auto* v = std::get_if<MetricFloat128>(&value)) std::copy(v->bytes.begin(), v->bytes.end(), bytes.begin());
  else if (const auto* v = std::get_if<MetricDecimal128>(&value)) std::copy(v->bytes.begin(), v->bytes.end(), bytes.begin());
  return bytes;
}
std::optional<MetricScalar> NumericValue(MetricScalarType type, std::span<const byte> bytes) {
  if (!Width(type) || bytes.size() != Width(type)) return {};
  MetricScalar value;
  switch (type) {
    case MetricScalarType::uint64: value = platform::LoadLittle64(bytes.data()); break;
    case MetricScalarType::int64: value = std::bit_cast<std::int64_t>(platform::LoadLittle64(bytes.data())); break;
    case MetricScalarType::float64: value = std::bit_cast<double>(platform::LoadLittle64(bytes.data())); break;
    case MetricScalarType::float128: {
      MetricFloat128 v; std::copy(bytes.begin(), bytes.end(), v.bytes.begin()); value = v; break;
    }
    case MetricScalarType::decimal128: {
      MetricDecimal128 v; std::copy(bytes.begin(), bytes.end(), v.bytes.begin()); value = v; break;
    }
    default: return {};
  }
  return MetricScalarValid(value) ? std::optional<MetricScalar>(std::move(value)) : std::nullopt;
}
bool Family(const CatalogMetadataVersionView& m) {
  return m.record.header.kind == CatalogRecordKind::metric_descriptor ||
      m.object_subtype == "metric_descriptor" || IsCatalogMetricDescriptorPayload(m.record.payload);
}
}  // namespace

namespace {
using T = CatalogValueType;
constexpr std::array<CatalogValueFieldSchema,32> kFields{{
      {1,T::engine_identity,true,16,UuidKind::object}, {2,T::unsigned_integer,true,8},
      {3,T::utf8_text,true,4096}, {4,T::utf8_text,true,4096},
      {5,T::unsigned_integer,true,8}, {6,T::unsigned_integer,true,8}, {7,T::unsigned_integer,true,8},
      {8,T::utf8_text,true,4096}, {9,T::utf8_text,true,16384},
      {10,T::engine_identity,false,16,UuidKind::object}, {11,T::unsigned_integer,true,8},
      {12,T::engine_identity,true,16,UuidKind::object}, {13,T::unsigned_integer,true,8},
      {14,T::engine_identity,true,16,UuidKind::object}, {15,T::unsigned_integer,true,8},
      {16,T::opaque_bytes,true,16}, {17,T::opaque_bytes,true,16},
      {18,T::opaque_bytes,true,65540}, {19,T::boolean,true,1}, {20,T::opaque_bytes,true,32772},
      {21,T::boolean,true,1}, {22,T::engine_identity,true,16,UuidKind::transaction},
      {23,T::unsigned_integer,true,8}, {24,T::engine_identity,false,16,UuidKind::object},
      {25,T::unsigned_integer,true,8}, {26,T::unsigned_integer,true,8},
      {27,T::unsigned_integer,true,8}, {28,T::utf8_text,true,4096},
      {29,T::utf8_text_list,true,65536}, {30,T::opaque_bytes,true,1024},
      {31,T::opaque_bytes,true,1024}, {32,T::utf8_text_list,true,16384}
}};
}
const CatalogValueSchema& CatalogMetricDescriptorSchema() {
  static const CatalogValueSchema schema{65544,1,{kFields.begin(),kFields.end()}};
  return schema;
}

CatalogValueEncodeResult EncodeCatalogMetricDescriptor(const CatalogMetricDescriptor& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  const auto& d = r.definition; const auto& b = r.binding;
  std::vector<CatalogValueField> fields; fields.reserve(32);
  const auto add = [&](u16 id, auto&& value) {
    using T = std::decay_t<decltype(value)>;
    fields.push_back({id, CatalogValue(std::in_place_type<T>, std::forward<decltype(value)>(value))});
  };
  const auto object = [&](u16 id, const MetricUuid& value) { add(id, TypedUuid{UuidKind::object, value}); };
  object(1,b.metric_uuid); add(2,b.descriptor_generation); add(3,d.family); add(4,d.namespace_path);
  add(5,Code(d.type,kClasses)); add(6,Code(d.value_type,kTypes)); add(7,Code(d.unit,kUnits));
  add(8,d.producer_owner); add(9,d.help);
  if (b.label_schema_generation) object(10,b.label_schema_uuid);
  add(11,b.label_schema_generation); object(12,b.retention_policy_uuid); add(13,b.retention_policy_generation);
  object(14,b.visibility_policy_uuid); add(15,b.visibility_policy_generation);
  add(16,d.min_value ? NumericBytes(*d.min_value) : std::vector<byte>{});
  add(17,d.max_value ? NumericBytes(*d.max_value) : std::vector<byte>{});
  std::vector<byte> bounds(4); platform::StoreLittle32(bounds.data(), static_cast<u32>(d.histogram_buckets.size()));
  for (const auto& value : d.histogram_buckets) {
    const auto bytes = NumericBytes(value); bounds.insert(bounds.end(),bytes.begin(),bytes.end());
  }
  add(18,std::move(bounds)); add(19,d.histogram_cumulative);
  std::vector<byte> codes(4 + 8*d.enum_values.size());
  platform::StoreLittle32(codes.data(),static_cast<u32>(d.enum_values.size()));
  for (std::size_t i=0;i<d.enum_values.size();++i) platform::StoreLittle64(codes.data()+4+8*i,d.enum_values[i]);
  add(20,std::move(codes)); add(21,d.cluster_only); add(22,r.origin_transaction_uuid); add(23,r.origin_local_transaction_id);
  if (b.rate_source_counter_generation) object(24,b.rate_source_counter_uuid);
  add(25,b.rate_source_counter_generation); add(26,d.rate_window_nanoseconds);
  add(27,Code(d.visibility,kVisibility)); add(28,d.security_family);
  std::vector<std::string> keys; std::vector<byte> flags,types;
  for (const auto& label : d.labels) {
    keys.push_back(label.key); flags.push_back(static_cast<byte>(label.required | (label.sensitive << 1)));
    types.push_back(static_cast<byte>(Code(label.value_type,kLabels)));
  }
  add(29,std::move(keys)); add(30,std::move(flags)); add(31,std::move(types)); add(32,d.aliases);
  return EncodeCatalogValueBlock(CatalogMetricDescriptorSchema(),fields);
}

metrics::MetricScalar CatalogMetricNumericSequenceView::operator[](std::size_t index) const {
  const auto width=Width(type_);
  return *NumericValue(type_,bytes_.subspan(index*width,width));
}
CatalogMetricDescriptorViewResult DecodeCatalogMetricDescriptorView(std::string_view bytes) {
  if (bytes.size()>kCatalogValueBlockMaxBytes) return {CatalogValueError::size_limit,{}};
  std::array<CatalogValueFieldView,32> backing;
  const auto decoded=DecodeCatalogValueBlockInto({65544,1,kFields},
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if (!decoded.ok()) return {decoded.error,{}};
  std::array<const CatalogValueFieldView*,33> f{};
  for (const auto& field:decoded.fields) f[field.id]=&field;
  const auto n=[&](std::size_t id){return *f[id]->unsigned_value();};
  const auto raw=[&](std::size_t id){return f[id]->bytes;};
  const auto text=[&](std::size_t id){const auto b=raw(id);return std::string_view(reinterpret_cast<const char*>(b.data()),b.size());};
  const auto object=[&](std::size_t id){return f[id]->identity()->value;};
  CatalogMetricDescriptorView r; auto& d=r.definition; auto& b=r.binding;
  if (!FromCode(n(5),kClasses,d.type) || !FromCode(n(6),kTypes,d.value_type) ||
      !FromCode(n(7),kUnits,d.unit) || !FromCode(n(27),kVisibility,d.visibility)) return {};
  b.metric_uuid=object(1); b.descriptor_generation=n(2); d.family=text(3); d.namespace_path=text(4);
  d.producer_owner=text(8); d.help=text(9);
  if (f[10]) b.label_schema_uuid=object(10);
  b.label_schema_generation=n(11); b.retention_policy_uuid=object(12); b.retention_policy_generation=n(13);
  b.visibility_policy_uuid=object(14); b.visibility_policy_generation=n(15);
  for (const auto id:{16,17}) if (!raw(id).empty()) {
    auto value=NumericValue(d.value_type,raw(id)); if (!value) return {};
    (id==16?d.min_value:d.max_value)=std::move(value);
  }
  const auto bounds=raw(18); if (bounds.size()<4) return {};
  const auto count=platform::LoadLittle32(bounds.data()); const auto width=Width(d.value_type);
  if (count>4096 || (count && !width) || bounds.size()!=4+count*width) return {};
  for (std::size_t i=0;i<count;++i)
    if (!NumericValue(d.value_type,bounds.subspan(4+i*width,width))) return {};
  d.histogram_buckets=CatalogMetricNumericSequenceView(bounds.subspan(4),d.value_type,count);
  d.histogram_cumulative=raw(19)[0]!=0;
  const auto codes=raw(20); if (codes.size()<4) return {};
  const auto code_count=platform::LoadLittle32(codes.data());
  if (code_count>4096 || codes.size()!=4+std::size_t(code_count)*8) return {};
  d.enum_values=CatalogMetricEnumSequenceView(codes.subspan(4));
  d.cluster_only=raw(21)[0]!=0; r.origin_transaction_uuid=*f[22]->identity();
  r.origin_local_transaction_id=n(23);
  if (f[24]) b.rate_source_counter_uuid=object(24);
  b.rate_source_counter_generation=n(25); d.rate_window_nanoseconds=n(26); d.security_family=text(28);
  const auto keys=raw(29),flags=raw(30),types=raw(31);
  const auto key_count=platform::LoadLittle32(keys.data());
  if (key_count>1024 || key_count!=flags.size() || key_count!=types.size()) return {};
  for (std::size_t i=0;i<key_count;++i)
    if ((flags[i]&~3u) || types[i]<1 || types[i]>3) return {};
  d.labels=CatalogMetricLabelSequenceView(keys.subspan(4),flags,types);
  const auto aliases=raw(32); const auto alias_count=platform::LoadLittle32(aliases.data());
  if (alias_count>1024) return {};
  d.aliases=CatalogMetricAliasSequenceView(aliases.subspan(4),alias_count);
  // The common codec has validated every text and list member's strict UTF8.
  if (!Valid(r)) return {};
  return {CatalogValueError::none,std::move(r)};
}
CatalogMetricDescriptorResult DecodeCatalogMetricDescriptor(std::string_view bytes) {
  const auto decoded=DecodeCatalogMetricDescriptorView(bytes);
  if (!decoded.ok()) return {decoded.error,{}};
  const auto& v=*decoded.record; const auto& s=v.definition;
  CatalogMetricDescriptor r; auto& d=r.definition;
  r.binding=v.binding; r.origin_transaction_uuid=v.origin_transaction_uuid;
  r.origin_local_transaction_id=v.origin_local_transaction_id;
  d.family=s.family; d.namespace_path=s.namespace_path; d.help=s.help;
  d.producer_owner=s.producer_owner; d.security_family=s.security_family;
  d.type=s.type; d.unit=s.unit; d.visibility=s.visibility; d.cluster_only=s.cluster_only;
  d.value_type=s.value_type; d.min_value=s.min_value; d.max_value=s.max_value;
  d.histogram_cumulative=s.histogram_cumulative; d.rate_window_nanoseconds=s.rate_window_nanoseconds;
  d.labels.reserve(s.labels.size());
  for (const auto label:s.labels)
    d.labels.push_back({std::string(label.key),label.required,label.sensitive,label.value_type});
  d.aliases.reserve(s.aliases.size());
  for (const auto alias:s.aliases) d.aliases.emplace_back(alias);
  d.histogram_buckets.reserve(s.histogram_buckets.size());
  for (std::size_t i=0;i<s.histogram_buckets.size();++i) d.histogram_buckets.push_back(s.histogram_buckets[i]);
  d.enum_values.reserve(s.enum_values.size());
  for (std::size_t i=0;i<s.enum_values.size();++i) d.enum_values.push_back(s.enum_values[i]);
  return {CatalogValueError::none,std::move(r)};
}
bool IsCatalogMetricDescriptorPayload(std::string_view bytes) {
  return bytes.size()>=kCatalogValueBlockHeaderBytes && bytes.substr(0,4)=="SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==65544;
}
bool CatalogMetricDescriptorMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind!=CatalogRecordKind::metric_descriptor || !Identity(r.header.object_uuid,UuidKind::object)) return false;
  const auto decoded=DecodeCatalogMetricDescriptorView(r.payload);
  return decoded.ok() && decoded.record->binding.metric_uuid==r.header.object_uuid.value;
}
bool CatalogMetricDescriptorMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogMetricDescriptorMatchesHeader(BorrowCatalogTypedRecord(r));
}
bool CatalogMetricDescriptorMatchesMetadata(const CatalogMetadataVersionView& m) {
  if (!CatalogMetricDescriptorMatchesHeader(m.record) || m.object_subtype!="metric_descriptor" ||
      !Identity(m.owning_schema_uuid,UuidKind::schema) || !Identity(m.record.header.parent_uuid,UuidKind::object) ||
      m.record.header.parent_uuid.value!=m.owning_schema_uuid.value ||
      !Identity(m.default_name_uuid,UuidKind::object) || !Identity(m.name_vector_uuid,UuidKind::object) ||
      !Identity(m.security_policy_uuid,UuidKind::object) || !Identity(m.creator_transaction_uuid,UuidKind::transaction)) return false;
  const auto decoded=DecodeCatalogMetricDescriptorView(m.record.payload); const auto& r=*decoded.record;
  return m.definition_version==r.binding.descriptor_generation && m.security_policy_uuid.value==r.binding.visibility_policy_uuid &&
      m.authority_scope==(r.definition.cluster_only?CatalogAuthorityScope::cluster:CatalogAuthorityScope::local) &&
      r.origin_local_transaction_id<=m.creator_local_transaction_id &&
      (m.definition_version!=1 || (r.origin_transaction_uuid.value==m.creator_transaction_uuid.value &&
                                 r.origin_local_transaction_id==m.creator_local_transaction_id));
}
bool CatalogMetricDescriptorPreservesOrigin(const CatalogMetadataVersionView& previous,const CatalogMetadataVersionView& successor) {
  if (!Family(previous) && !Family(successor)) return true;
  if (!CatalogMetricDescriptorMatchesMetadata(previous) || !CatalogMetricDescriptorMatchesMetadata(successor)) return false;
  const auto a=DecodeCatalogMetricDescriptorView(previous.record.payload), b=DecodeCatalogMetricDescriptorView(successor.record.payload);
  return a.record->binding.metric_uuid==b.record->binding.metric_uuid && a.record->definition.cluster_only==b.record->definition.cluster_only &&
      a.record->origin_transaction_uuid.value==b.record->origin_transaction_uuid.value &&
      a.record->origin_local_transaction_id==b.record->origin_local_transaction_id;
}
bool CatalogMetricDescriptorMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogMetricDescriptorMatchesMetadata(BorrowCatalogMetadataVersion(m));
}
bool CatalogMetricDescriptorPreservesOrigin(const CatalogMetadataVersion& a, const CatalogMetadataVersion& b) {
  return CatalogMetricDescriptorPreservesOrigin(BorrowCatalogMetadataVersion(a), BorrowCatalogMetadataVersion(b));
}
}  // namespace scratchbird::core::catalog
