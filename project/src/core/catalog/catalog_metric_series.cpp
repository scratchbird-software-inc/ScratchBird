// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_series.hpp"
#include "metric_label_key.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace scratchbird::core::catalog {
namespace {
bool Identity(const TypedUuid& v, UuidKind kind) {
  return v.kind == kind && uuid::IsEngineIdentityUuid(v.value);
}
bool Optional(const Uuid& id, u64 generation) {
  return generation ? uuid::IsEngineIdentityUuid(id) : id.is_nil();
}
u8 Code(metrics::MetricLabelType type) {
  switch (type) {
    case metrics::MetricLabelType::text: return 1;
    case metrics::MetricLabelType::system_uuid: return 2;
    case metrics::MetricLabelType::uuid_value: return 3;
  }
  return 0;
}
bool Less(std::string_view a, std::string_view b) {
  return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
      [](unsigned char x,unsigned char y){return x<y;});
}
auto Ordered(const std::vector<CatalogMetricSeriesLabel>& labels) {
  auto result=labels;
  std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return Less(a.key,b.key);});
  return result;
}
std::optional<std::string_view> LabelText(const CatalogMetricSeriesLabel& label) {
  const auto* text=std::get_if<std::string>(&label.value);
  return text?std::optional<std::string_view>(*text):std::nullopt;
}
std::optional<std::string_view> LabelText(const CatalogMetricSeriesLabelView& label) {
  const auto* text=std::get_if<std::string_view>(&label.value);
  return text?std::optional<std::string_view>(*text):std::nullopt;
}
template<class R> bool Valid(const R& r) {
  const auto& b=r.binding;
  if (!uuid::IsEngineIdentityUuid(r.series_uuid) || !r.generation ||
      !uuid::IsEngineIdentityUuid(b.database_uuid) || !uuid::IsEngineIdentityUuid(b.node_uuid) ||
      (!b.cluster_uuid.is_nil() && !uuid::IsEngineIdentityUuid(b.cluster_uuid)) ||
      !uuid::IsEngineIdentityUuid(b.metric_uuid) || !b.descriptor_generation ||
      !uuid::IsEngineIdentityUuid(b.retention_policy_uuid) || !b.retention_policy_generation ||
      !uuid::IsEngineIdentityUuid(b.visibility_policy_uuid) || !b.visibility_policy_generation ||
      !Optional(b.label_schema_uuid,b.label_schema_generation) ||
      !Optional(b.rate_source_counter_uuid,b.rate_source_counter_generation) ||
      (!b.label_schema_generation && !r.labels.empty()) ||
      !Identity(r.origin_transaction_uuid,UuidKind::transaction) || !r.origin_local_transaction_id ||
      r.labels.size()>1024) return false;
  std::size_t keys=4,values=4;
  for (auto it=r.labels.begin();it!=r.labels.end();++it) {
    const auto& l=*it;
    if (l.key.empty() || l.key.size()>4096 || l.key.find('\0')!=l.key.npos ||
        !metrics::MetricTextValid(l.key) || !Code(l.type)) return false;
    if ((metrics::MetricOwnerLabel(l.key) && l.type!=metrics::MetricLabelType::system_uuid) ||
        !metrics::MetricOwnerLabelMatchesScope(l.key,std::get_if<Uuid>(&l.value),b.database_uuid,b.node_uuid,b.cluster_uuid))
      return false;
    for (auto prior=r.labels.begin();prior!=it;++prior) if((*prior).key==l.key) return false;
    std::size_t size=16;
    if (l.type==metrics::MetricLabelType::text) {
      const auto text=LabelText(l);
      if (!text || text->empty() || text->size()>65536 || !metrics::MetricTextValid(*text)) return false;
      size=text->size();
    } else {
      const auto* id=std::get_if<Uuid>(&l.value);
      if (!id || (l.type==metrics::MetricLabelType::system_uuid ? !uuid::IsEngineIdentityUuid(*id) :
          ((id->bytes[8]&0xc0)!=0x80 || (id->bytes[6]>>4)<1 || (id->bytes[6]>>4)>7))) return false;
    }
    keys+=4+l.key.size();values+=4+size;
    if(keys>65536 || values>65536) return false;
  }
  return true;
}
bool Family(const CatalogMetadataVersion& m) {
  return m.record.header.kind==CatalogRecordKind::metric_series || m.object_subtype=="metric_series" ||
      IsCatalogMetricSeriesPayload(m.record.payload);
}
}  // namespace
namespace {
using T=CatalogValueType;
constexpr std::array<CatalogValueFieldSchema,20> kSeriesFields{{
      {1,T::engine_identity,true,16,UuidKind::object},{2,T::unsigned_integer,true,8},
      {3,T::engine_identity,true,16,UuidKind::database},{4,T::engine_identity,true,16,UuidKind::object},
      {5,T::user_uuid_data,true,16},{6,T::engine_identity,true,16,UuidKind::object},
      {7,T::unsigned_integer,true,8},{8,T::user_uuid_data,true,16},{9,T::unsigned_integer,true,8},
      {10,T::engine_identity,true,16,UuidKind::object},{11,T::unsigned_integer,true,8},
      {12,T::engine_identity,true,16,UuidKind::object},{13,T::unsigned_integer,true,8},
      {14,T::user_uuid_data,true,16},{15,T::unsigned_integer,true,8},
      {16,T::utf8_text_list,true,65536},{17,T::opaque_bytes,true,1024},{18,T::opaque_bytes,true,65536},
      {19,T::engine_identity,true,16,UuidKind::transaction},{20,T::unsigned_integer,true,8}
}};
}  // namespace
const CatalogValueSchema& CatalogMetricSeriesSchema() {
  static const CatalogValueSchema schema{65546,1,{kSeriesFields.begin(),kSeriesFields.end()}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogMetricSeries(const CatalogMetricSeries& r) {
  if(!Valid(r))return {CatalogValueError::invalid_value,{}};
  const auto labels=Ordered(r.labels);
  std::vector<std::string> keys;std::vector<byte> types,values(4);
  platform::StoreLittle32(values.data(),static_cast<u32>(labels.size()));
  for(const auto& l:labels) {
    keys.push_back(l.key);types.push_back(Code(l.type));
    const auto* text=std::get_if<std::string>(&l.value);
    const auto size=text?text->size():16;
    const auto at=values.size();values.resize(at+4+size);
    platform::StoreLittle32(values.data()+at,static_cast<u32>(size));
    if(text)std::copy(text->begin(),text->end(),values.begin()+at+4);
    else {const auto& id=std::get<Uuid>(l.value);std::copy(id.bytes.begin(),id.bytes.end(),values.begin()+at+4);}
  }
  const auto& b=r.binding;
  std::vector<CatalogValueField> fields;
  fields.reserve(20);
  fields.push_back({1,TypedUuid{UuidKind::object,r.series_uuid}});
  fields.push_back({2,r.generation});
  fields.push_back({3,TypedUuid{UuidKind::database,b.database_uuid}});
  fields.push_back({4,TypedUuid{UuidKind::object,b.node_uuid}});
  fields.push_back({5,b.cluster_uuid});
  fields.push_back({6,TypedUuid{UuidKind::object,b.metric_uuid}});
  fields.push_back({7,b.descriptor_generation});
  fields.push_back({8,b.label_schema_uuid});
  fields.push_back({9,b.label_schema_generation});
  fields.push_back({10,TypedUuid{UuidKind::object,b.retention_policy_uuid}});
  fields.push_back({11,b.retention_policy_generation});
  fields.push_back({12,TypedUuid{UuidKind::object,b.visibility_policy_uuid}});
  fields.push_back({13,b.visibility_policy_generation});
  fields.push_back({14,b.rate_source_counter_uuid});
  fields.push_back({15,b.rate_source_counter_generation});
  fields.push_back({16,std::move(keys)});
  fields.push_back({17,std::move(types)});
  fields.push_back({18,std::move(values)});
  fields.push_back({19,r.origin_transaction_uuid});
  fields.push_back({20,r.origin_local_transaction_id});
  return EncodeCatalogValueBlock(CatalogMetricSeriesSchema(),fields);
}
CatalogMetricSeriesLabelsView::Iterator::value_type
CatalogMetricSeriesLabelsView::Iterator::operator*() const {
  const auto key_size=platform::LoadLittle32(keys_), value_size=platform::LoadLittle32(values_);
  value_type label;
  label.key={reinterpret_cast<const char*>(keys_+4),key_size};
  if (*types_==1) {
    label.type=metrics::MetricLabelType::text;
    label.value=std::string_view(reinterpret_cast<const char*>(values_+4),value_size);
  } else {
    label.type=*types_==2?metrics::MetricLabelType::system_uuid:metrics::MetricLabelType::uuid_value;
    Uuid id;std::copy_n(values_+4,16,id.bytes.begin());label.value=id;
  }
  return label;
}
CatalogMetricSeriesLabelsView::Iterator& CatalogMetricSeriesLabelsView::Iterator::operator++() {
  keys_+=4+platform::LoadLittle32(keys_);values_+=4+platform::LoadLittle32(values_);++types_;return *this;
}
bool CatalogMetricSeriesLabelsView::operator==(const CatalogMetricSeriesLabelsView& other) const {
  return size()==other.size()&&std::equal(begin(),end(),other.begin());
}
CatalogMetricSeriesViewResult DecodeCatalogMetricSeriesView(std::string_view bytes) {
  if(bytes.size()>kCatalogValueBlockMaxBytes)return {CatalogValueError::size_limit,{}};
  std::array<CatalogValueFieldView,20> backing;
  const auto decoded=DecodeCatalogValueBlockInto({65546,1,kSeriesFields},
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!decoded.ok())return {decoded.error,{}};
  const auto& f=decoded.fields;
  const auto raw_uuid=[&](unsigned at) { Uuid id;std::copy_n(f[at].bytes.data(),16,id.bytes.begin());return id; };
  CatalogMetricSeriesView r;auto& b=r.binding;
  r.series_uuid=f[0].identity()->value;r.generation=*f[1].unsigned_value();
  b.database_uuid=f[2].identity()->value;b.node_uuid=f[3].identity()->value;
  b.cluster_uuid=raw_uuid(4);b.metric_uuid=f[5].identity()->value;
  b.descriptor_generation=*f[6].unsigned_value();b.label_schema_uuid=raw_uuid(7);
  b.label_schema_generation=*f[8].unsigned_value();b.retention_policy_uuid=f[9].identity()->value;
  b.retention_policy_generation=*f[10].unsigned_value();b.visibility_policy_uuid=f[11].identity()->value;
  b.visibility_policy_generation=*f[12].unsigned_value();b.rate_source_counter_uuid=raw_uuid(13);
  b.rate_source_counter_generation=*f[14].unsigned_value();
  r.origin_transaction_uuid=*f[18].identity();r.origin_local_transaction_id=*f[19].unsigned_value();
  const auto keys=f[15].bytes,types=f[16].bytes,values=f[17].bytes;
  const auto count=platform::LoadLittle32(keys.data());
  if(count>1024 || count!=types.size() || values.size()<4 || platform::LoadLittle32(values.data())!=count)return {};
  std::size_t key_at=4,value_at=4;std::string_view previous;
  for(std::size_t i=0;i<count;++i) {
    // Key lengths and exact consumption have already passed the common codec.
    const auto key_size=platform::LoadLittle32(keys.data()+key_at);key_at+=4;
    const std::string_view key(reinterpret_cast<const char*>(keys.data()+key_at),key_size);key_at+=key_size;
    if((i && !Less(previous,key)) || values.size()-value_at<4)return {};
    previous=key;
    const auto size=platform::LoadLittle32(values.data()+value_at);value_at+=4;
    if(size>values.size()-value_at)return {};
    if(types[i]!=1 && ((types[i]!=2 && types[i]!=3) || size!=16))return {};
    value_at+=size;
  }
  if(value_at!=values.size())return {};
  r.labels=CatalogMetricSeriesLabelsView(keys.subspan(4),types,values.subspan(4));
  if(!Valid(r))return {};
  return {CatalogValueError::none,std::move(r)};
}
CatalogMetricSeriesResult DecodeCatalogMetricSeries(std::string_view bytes) {
  const auto decoded=DecodeCatalogMetricSeriesView(bytes);
  if(!decoded.ok())return {decoded.error,{}};
  const auto& v=*decoded.record;
  CatalogMetricSeries r{v.series_uuid,v.generation,v.binding,{},v.origin_transaction_uuid,v.origin_local_transaction_id};
  r.labels.reserve(v.labels.size());
  for(const auto label:v.labels) {
    metrics::MetricLabelValue value;
    if(const auto* text=std::get_if<std::string_view>(&label.value))value=std::string(*text);
    else value=std::get<Uuid>(label.value);
    r.labels.push_back({std::string(label.key),label.type,std::move(value)});
  }
  return {CatalogValueError::none,std::move(r)};
}
bool IsCatalogMetricSeriesPayload(std::string_view bytes) {
  return bytes.size()>=kCatalogValueBlockHeaderBytes && bytes.substr(0,4)=="SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==65546;
}
bool CatalogMetricSeriesMatchesHeader(const CatalogTypedRecord& r) {
  if(r.header.kind!=CatalogRecordKind::metric_series || !Identity(r.header.object_uuid,UuidKind::object))return false;
  const auto decoded=DecodeCatalogMetricSeriesView(r.payload);
  return decoded.ok() && decoded.record->series_uuid==r.header.object_uuid.value;
}
bool CatalogMetricSeriesMatchesMetadata(const CatalogMetadataVersion& m) {
  if(!CatalogMetricSeriesMatchesHeader(m.record) || m.object_subtype!="metric_series" ||
      !Identity(m.owning_schema_uuid,UuidKind::schema) || !Identity(m.record.header.parent_uuid,UuidKind::object) ||
      m.owning_schema_uuid.value!=m.record.header.parent_uuid.value ||
      !Identity(m.default_name_uuid,UuidKind::object) || !Identity(m.name_vector_uuid,UuidKind::object) ||
      !Identity(m.security_policy_uuid,UuidKind::object) || !Identity(m.creator_transaction_uuid,UuidKind::transaction))return false;
  const auto decoded=DecodeCatalogMetricSeriesView(m.record.payload);const auto& r=*decoded.record;
  return r.generation==m.definition_version &&
      m.authority_scope==(r.binding.cluster_uuid.is_nil()?CatalogAuthorityScope::local:CatalogAuthorityScope::cluster) &&
      r.origin_local_transaction_id<=m.creator_local_transaction_id &&
      (r.generation!=1 || (r.origin_transaction_uuid.value==m.creator_transaction_uuid.value &&
                          r.origin_local_transaction_id==m.creator_local_transaction_id));
}
bool CatalogMetricSeriesPreservesOrigin(const CatalogMetadataVersion& previous,const CatalogMetadataVersion& successor) {
  if(!Family(previous) && !Family(successor))return true;
  if(!CatalogMetricSeriesMatchesMetadata(previous) || !CatalogMetricSeriesMatchesMetadata(successor))return false;
  const auto a=DecodeCatalogMetricSeriesView(previous.record.payload),b=DecodeCatalogMetricSeriesView(successor.record.payload);
  const auto& x=*a.record;const auto& y=*b.record;
  return x.series_uuid==y.series_uuid && x.binding.database_uuid==y.binding.database_uuid &&
      x.binding.node_uuid==y.binding.node_uuid && x.binding.cluster_uuid==y.binding.cluster_uuid &&
      x.binding.metric_uuid==y.binding.metric_uuid && x.binding.label_schema_uuid==y.binding.label_schema_uuid &&
      x.labels==y.labels && x.origin_transaction_uuid.value==y.origin_transaction_uuid.value &&
      x.origin_local_transaction_id==y.origin_local_transaction_id;
}
metrics::MetricHistoryRecordResult<metrics::MetricSeriesIdentity> BindCatalogMetricSeries(
    const CatalogMetricSeries& r,const metrics::MetricDescriptor& d,const metrics::MetricRetentionPolicy& policy) {
  if(!Valid(r))return {};
  metrics::MetricLabelSet labels;
  for(const auto& l:r.labels) {
    const auto schema=std::find_if(d.labels.begin(),d.labels.end(),[&](const auto& s){return s.key==l.key;});
    if(schema==d.labels.end() || schema->value_type!=l.type)return {};
    labels.push_back({l.key,l.value});
  }
  return metrics::MakeMetricSeriesIdentity(d,std::move(labels),policy,r.binding,r.series_uuid,r.generation);
}
}  // namespace scratchbird::core::catalog
