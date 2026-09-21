// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_sample_codec.hpp"
#include "metric_label_key.hpp"
#include <algorithm>
#include <cstring>
#include <new>
#include <stdexcept>

namespace scratchbird::core::metrics {
namespace {
using platform::byte;
using E=MetricSampleCodecError;
bool Quality(const std::string& s) {
  return s.size()<=128&&s.find('\0')==s.npos&&MetricScalarValid(MetricScalar(s));
}
bool LabelsBounded(const MetricLabelSet& labels) {
  if(labels.size()>1024)return false;
  std::size_t bytes=0;
  for(const auto& l:labels) {
    if(l.key.size()>4096)return false;
    const auto* text=std::get_if<std::string>(&l.value);
    const auto size=text?text->size():16;
    if(size>kMetricValueMaxBytes||l.key.size()>kMetricValueMaxBytes-size)return false;
    const auto field=size+l.key.size();
    if(field>kMetricValueMaxBytes-bytes)return false;
    bytes+=field;
  }
  return true;
}
MetricHistorySeriesKey Key(const MetricHistoryBinding& b,const MetricLabelSet& labels) {
  return {b.database_uuid,b.node_uuid,b.cluster_uuid,b.metric_uuid,b.label_schema_uuid,
          MakeMetricSeriesKey({},labels).second};
}
E Validate(const MetricDescriptor& d,const MetricSeriesIdentity& s,const MetricRawSampleRecord& r) {
  if(!LabelsBounded(s.labels)||!LabelsBounded(r.labels)||!LabelsBounded(r.value.labels))return E::size_limit;
  if(!MetricDescriptorReferencesValid(d,d)||
      static_cast<const MetricDescriptorBinding&>(s)!=static_cast<const MetricDescriptorBinding&>(d)||
      static_cast<const MetricHistoryBinding&>(s)!=static_cast<const MetricHistoryBinding&>(r)||
      !MetricSystemUuidValid(s.database_uuid)||!MetricSystemUuidValid(s.node_uuid)||
      (d.cluster_only?!MetricSystemUuidValid(s.cluster_uuid):!s.cluster_uuid.is_nil())||
      !MetricSystemUuidValid(s.series_uuid)||r.series_uuid!=s.series_uuid||
      !s.series_definition_generation||r.series_definition_generation!=s.series_definition_generation||
      s.metric_family!=d.family||r.metric_family!=d.family||
      s.scope_class!=(d.cluster_only?"cluster":"local")||
      s.series_key!=Key(s,s.labels)||s.series_key!=Key(r,r.labels)||s.series_key!=Key(r,r.value.labels))
    return E::invalid_binding;
  if(!MetricSystemUuidValid(r.sample_uuid)||(!r.evidence_uuid.is_nil()&&!MetricSystemUuidValid(r.evidence_uuid))||
      !r.sample_time_utc_ns||!r.collection_time_utc_ns||!r.source_sequence||!r.revision||
      !Quality(r.clock_quality)||!Quality(r.freshness_class))return E::invalid_sample;
  return E::none;
}
E ValueError(MetricValueCodecError e) {
  if(e==MetricValueCodecError::resource_exhausted)return E::resource_exhausted;
  if(e==MetricValueCodecError::size_limit)return E::size_limit;
  return E::invalid_value;
}
}
MetricSampleEncodeResult EncodeMetricRawSample(const MetricDescriptor& d,
    const MetricSeriesIdentity& s,const MetricRawSampleRecord& r) noexcept {
  try {
    const auto valid=Validate(d,s,r);if(valid!=E::none)return {valid,{}};
    auto value=EncodeMetricValue(d,r.value);if(!value.ok())return {ValueError(value.error),{}};
    const auto total=kMetricSampleHeaderBytes+r.clock_quality.size()+r.freshness_class.size()+value.bytes.size();
    if(total>kMetricSampleMaxBytes)return {E::size_limit,{}};
    std::vector<byte> b(total);
    const auto id=[&](std::size_t at,const MetricUuid& u){std::copy(u.bytes.begin(),u.bytes.end(),b.begin()+at);};
    const auto n=[&](std::size_t at,u64 v){platform::StoreLittle64(b.data()+at,v);};
    std::memcpy(b.data(),"SBMS",4);platform::StoreLittle16(b.data()+4,2);
    platform::StoreLittle16(b.data()+6,kMetricSampleHeaderBytes);
    platform::StoreLittle32(b.data()+8,static_cast<platform::u32>(total));
    platform::StoreLittle32(b.data()+12,static_cast<platform::u32>(value.bytes.size()));
    id(16,r.sample_uuid);id(32,r.series_uuid);id(48,r.metric_uuid);n(64,r.descriptor_generation);
    id(72,r.label_schema_uuid);n(88,r.label_schema_generation);
    id(96,r.retention_policy_uuid);n(112,r.retention_policy_generation);
    id(120,r.visibility_policy_uuid);n(136,r.visibility_policy_generation);
    id(144,r.rate_source_counter_uuid);n(160,r.rate_source_counter_generation);
    id(168,r.database_uuid);id(184,r.node_uuid);id(200,r.cluster_uuid);id(216,r.evidence_uuid);
    n(232,r.sample_time_utc_ns);n(240,r.collection_time_utc_ns);n(248,r.publication_time_utc_ns);
    n(256,r.source_sequence);n(264,r.revision);
    platform::StoreLittle32(b.data()+272,static_cast<platform::u32>(r.clock_quality.size()));
    platform::StoreLittle32(b.data()+276,static_cast<platform::u32>(r.freshness_class.size()));
    n(280,r.series_definition_generation);
    auto end=std::copy(r.clock_quality.begin(),r.clock_quality.end(),b.begin()+kMetricSampleHeaderBytes);
    end=std::copy(r.freshness_class.begin(),r.freshness_class.end(),end);
    std::copy(value.bytes.begin(),value.bytes.end(),end);
    return {E::none,std::move(b)};
  }catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}
   catch(...){return {};}
}
MetricSampleDecodeResult DecodeMetricRawSample(const MetricDescriptor& d,
    const MetricSeriesIdentity& s,std::span<const byte> b) noexcept {
  try {
    if(b.size()>kMetricSampleMaxBytes)return {E::size_limit,{}};
    if(b.size()<8||std::memcmp(b.data(),"SBMS",4))return {E::invalid_framing,{}};
    if(platform::LoadLittle16(b.data()+4)!=2)return {E::unsupported_version,{}};
    if(b.size()<kMetricSampleHeaderBytes||
        platform::LoadLittle16(b.data()+6)!=kMetricSampleHeaderBytes||
        platform::LoadLittle32(b.data()+8)!=b.size())return {E::invalid_framing,{}};
    const auto size=platform::LoadLittle32(b.data()+12);
    const auto clock=platform::LoadLittle32(b.data()+272),fresh=platform::LoadLittle32(b.data()+276);
    if(size>kMetricValueMaxBytes||clock>128||fresh>128)return {E::size_limit,{}};
    if(size<kMetricValueHeaderBytes||b.size()!=kMetricSampleHeaderBytes+std::size_t(clock)+fresh+size)
      return {E::invalid_framing,{}};
    MetricRawSampleRecord r;
    const auto id=[&](std::size_t at,MetricUuid& u){std::copy_n(b.begin()+at,16,u.bytes.begin());};
    const auto n=[&](std::size_t at){return platform::LoadLittle64(b.data()+at);};
    id(16,r.sample_uuid);id(32,r.series_uuid);id(48,r.metric_uuid);r.descriptor_generation=n(64);
    id(72,r.label_schema_uuid);r.label_schema_generation=n(88);
    id(96,r.retention_policy_uuid);r.retention_policy_generation=n(112);
    id(120,r.visibility_policy_uuid);r.visibility_policy_generation=n(136);
    id(144,r.rate_source_counter_uuid);r.rate_source_counter_generation=n(160);
    id(168,r.database_uuid);id(184,r.node_uuid);id(200,r.cluster_uuid);id(216,r.evidence_uuid);
    r.sample_time_utc_ns=n(232);r.collection_time_utc_ns=n(240);r.publication_time_utc_ns=n(248);
    r.source_sequence=n(256);r.revision=n(264);r.metric_family=d.family;
    r.series_definition_generation=n(280);
    r.clock_quality.assign(reinterpret_cast<const char*>(b.data()+kMetricSampleHeaderBytes),clock);
    r.freshness_class.assign(reinterpret_cast<const char*>(b.data()+kMetricSampleHeaderBytes+clock),fresh);
    auto value=DecodeMetricValue(d,b.subspan(kMetricSampleHeaderBytes+clock+fresh,size));
    if(!value.ok())return {ValueError(value.error),{}};
    r.value=std::move(*value.value);r.labels=r.value.labels;
    const auto valid=Validate(d,s,r);if(valid!=E::none)return {valid,{}};
    return {E::none,std::move(r)};
  }catch(const std::bad_alloc&){return {E::resource_exhausted,{}};}
   catch(const std::length_error&){return {E::resource_exhausted,{}};}
   catch(...){return {};}
}
}  // namespace scratchbird::core::metrics
