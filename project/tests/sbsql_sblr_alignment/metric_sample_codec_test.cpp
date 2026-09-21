// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define METRIC_VALUE_CODEC_MAIN MetricValueCodecBaseMain
#include "metric_value_codec_test.cpp"
#undef METRIC_VALUE_CODEC_MAIN
#include "metric_sample_codec.hpp"
#include "metric_label_key.hpp"

namespace {
struct SampleFixture {
  m::MetricDescriptor descriptor;
  m::MetricSeriesIdentity series;
  m::MetricRawSampleRecord sample;
};
SampleFixture Sample(m::MetricScalar value=U(9007199254740993ULL)) {
  SampleFixture f;auto& d=f.descriptor;
  d=Descriptor(m::MetricScalarTypeOf(value));d.type=m::MetricType::sample;
  if(d.value_type==T::enumeration)d.enum_values={U(-1)};
  d.metric_uuid=Id(1);d.descriptor_generation=2;d.label_schema_uuid=Id(3);d.label_schema_generation=4;
  d.retention_policy_uuid=Id(5);d.retention_policy_generation=6;d.visibility_policy_uuid=Id(7);d.visibility_policy_generation=8;
  d.labels={{"object",true,true,m::MetricLabelType::system_uuid},{"data",true,false,m::MetricLabelType::uuid_value},
    {"text",false,false,m::MetricLabelType::text}};
  m::MetricHistoryBinding binding;static_cast<m::MetricDescriptorBinding&>(binding)=d;
  binding.database_uuid=Id(9);binding.node_uuid=Id(10);
  m::MetricRetentionPolicy policy;policy.policy_uuid=d.retention_policy_uuid;policy.generation=6;policy.policy_name="sample-policy";
  m::MetricLabelSet labels={{"object",Id(11)},{"data",Id(12,4)},{"text",std::string("x=;\0\xc3\xa9",7)}};
  auto series=m::MakeMetricSeriesIdentity(d,labels,policy,binding,Id(13));Check(series.ok(),"real series factory");
  if(!series.ok())throw "series factory failed";
  f.series=std::move(*series.record);auto v=Value(d,std::move(value));v.labels=labels;
  auto sample=m::MakeMetricRawSampleRecord(d,f.series,v,U(-1),U(-2),U(-3));Check(sample.ok(),"real sample factory");
  if(!sample.ok())throw "sample factory failed";
  f.sample=std::move(*sample.record);return f;
}
Bytes GoldenSample(const SampleFixture& f) {
  const auto& s=f.sample;Bytes b(280,0);b[0]='S';b[1]='B';b[2]='M';b[3]='S';Put(b,4,1,2);Put(b,6,280,2);
  const auto id=[&](std::size_t at,const m::MetricUuid& u){std::copy(u.bytes.begin(),u.bytes.end(),b.begin()+at);};
  id(16,s.sample_uuid);id(32,s.series_uuid);id(48,s.metric_uuid);Put(b,64,s.descriptor_generation,8);
  id(72,s.label_schema_uuid);Put(b,88,s.label_schema_generation,8);id(96,s.retention_policy_uuid);Put(b,112,s.retention_policy_generation,8);
  id(120,s.visibility_policy_uuid);Put(b,136,s.visibility_policy_generation,8);
  id(144,s.rate_source_counter_uuid);Put(b,160,s.rate_source_counter_generation,8);
  id(168,s.database_uuid);id(184,s.node_uuid);id(200,s.cluster_uuid);id(216,s.evidence_uuid);
  Put(b,232,s.sample_time_utc_ns,8);Put(b,240,s.collection_time_utc_ns,8);Put(b,248,s.publication_time_utc_ns,8);
  Put(b,256,s.source_sequence,8);Put(b,264,s.revision,8);Put(b,272,s.clock_quality.size(),4);Put(b,276,s.freshness_class.size(),4);
  b.insert(b.end(),s.clock_quality.begin(),s.clock_quality.end());b.insert(b.end(),s.freshness_class.begin(),s.freshness_class.end());
  auto value=Golden(f.descriptor,s.value);Put(b,12,value.size(),4);b.insert(b.end(),value.begin(),value.end());Put(b,8,b.size(),4);return b;
}
void SampleRejected(const SampleFixture& f,const Bytes& b) {
  auto r=m::DecodeMetricRawSample(f.descriptor,f.series,b);Check(!r.ok()&&!r.record,"invalid native sample decoded");
}
void SampleEncodeRejected(const SampleFixture& f) {
  auto r=m::EncodeMetricRawSample(f.descriptor,f.series,f.sample);Check(!r.ok()&&r.bytes.empty(),"invalid native sample encoded");
}
void SampleRoundTrip(const SampleFixture& f,bool prefixes=true) {
  auto b=GoldenSample(f);auto e=m::EncodeMetricRawSample(f.descriptor,f.series,f.sample);
  Check(e.ok()&&e.bytes==b,"sample bytes differ from independent layout");
  auto r=m::DecodeMetricRawSample(f.descriptor,f.series,b);Check(r.ok(),"independent native sample rejected");
  if(r.ok()) {
    auto decoded=f;decoded.sample=std::move(*r.record);
    Check(GoldenSample(decoded)==b,"sample decode lost exact bindings/times/value");
    Check(decoded.sample.sample_uuid==f.sample.sample_uuid&&decoded.sample.publication_time_utc_ns==f.sample.publication_time_utc_ns&&
      decoded.sample.clock_quality==f.sample.clock_quality,"decode fabricated identity/publication/quality");
  }
  if(prefixes)for(std::size_t n=0;n<b.size();++n) {
    auto r=m::DecodeMetricRawSample(f.descriptor,f.series,std::span(b).first(n));
    Check(!r.ok()&&!r.record,"truncated native sample decoded");
  }
}
void SampleTypesAndBindings() {
  for(const auto& v:std::vector<m::MetricScalar>{U(-1),std::numeric_limits<I>::min(),-0.,
      Binary("1e4000"),Decimal("9999999999999999999999999999999999",6111),true,
      std::string("x\0y",3),Id(30,1),m::MetricEnumValue{U(-1)}})SampleRoundTrip(Sample(v));
  auto f=Sample();const auto original=GoldenSample(f);
  for(auto at:{32u,48u,72u,96u,120u,144u,168u,184u,200u}) {
    auto bad=original;bad[at+15]^=1;SampleRejected(f,bad);
  }
  for(auto at:{64u,88u,112u,136u,160u}) {
    auto bad=original;Put(bad,at,0,8);
    if(at==160)Put(bad,at,1,8);
    SampleRejected(f,bad);bad=original;bad[at]^=0x80;SampleRejected(f,bad);
  }
  for(auto at:{232u,240u,256u,264u}){auto bad=original;Put(bad,at,0,8);SampleRejected(f,bad);}
  for(unsigned version=0;version<16;++version)if(version!=7) {
    auto bad=original;bad[22]=version<<4;SampleRejected(f,bad);
  }
  for(unsigned variant:{0u,0x40u,0xc0u}){auto bad=original;bad[24]=variant;SampleRejected(f,bad);}
  for(auto at:{0u,6u,8u,12u}){auto bad=original;bad[at]^=0x80;SampleRejected(f,bad);}
  auto bad=original;Put(bad,4,2,2);SampleRejected(f,bad);
  for(auto at:{12u,272u,276u}){bad=original;Put(bad,at,0xffffffff,4);SampleRejected(f,bad);}
  bad=original;bad.push_back(0);Put(bad,8,bad.size(),4);SampleRejected(f,bad);
  f.sample.evidence_uuid=Id(20,4);SampleEncodeRejected(f);
  f.sample.evidence_uuid=Id(20);f.sample.publication_time_utc_ns=U(-4);f.sample.revision=U(-1);
  f.sample.clock_quality="retained-quality";f.sample.freshness_class="retained-freshness";SampleRoundTrip(f);
  f.sample.clock_quality=std::string("\xc0\x80",2);SampleEncodeRejected(f);
  f.sample.clock_quality=std::string("x\0y",3);SampleEncodeRejected(f);
  f.sample.clock_quality=std::string(129,'x');SampleEncodeRejected(f);
  f=Sample();f.sample.labels[0].value=Id(99);SampleEncodeRejected(f);
  f=Sample();f.sample.value.labels[0].value=Id(99);SampleEncodeRejected(f);
  f=Sample();f.series.series_key={};SampleEncodeRejected(f);SampleRejected(f,original);
  f=Sample();f.series.database_uuid=Id(99);SampleEncodeRejected(f);
  f=Sample();f.descriptor.descriptor_generation++;SampleEncodeRejected(f);
  f=Sample();f.sample.metric_family="different";SampleEncodeRejected(f);
  f=Sample();f.series.scope_class="cluster";SampleEncodeRejected(f);
  f=Sample();f.sample.labels[2].value=std::string(1048577,'x');SampleEncodeRejected(f);
  // Cluster-shaped retained data is not a cluster provider or membership grant.
  f=Sample();f.descriptor.cluster_only=true;f.series.scope_class="cluster";f.series.cluster_uuid=Id(31);f.sample.cluster_uuid=Id(31);
  std::get<2>(f.series.series_key)=Id(31);SampleRoundTrip(f);
  f.sample.cluster_uuid={};SampleEncodeRejected(f);
  f=Sample();f.descriptor.labels.clear();f.descriptor.label_schema_uuid={};f.descriptor.label_schema_generation=0;
  f.series.labels.clear();f.sample.labels.clear();f.sample.value.labels.clear();
  f.series.label_schema_uuid={};f.series.label_schema_generation=0;f.sample.label_schema_uuid={};f.sample.label_schema_generation=0;
  std::get<4>(f.series.series_key)={};std::get<5>(f.series.series_key).clear();SampleRoundTrip(f);
}
void SampleWireMutations() {
  const auto f=Sample();const auto original=GoldenSample(f);
  unsigned accepted=0,refused=0;
  for(std::size_t at=0;at<original.size();++at)for(unsigned bit=0;bit<8;++bit) {
    auto bytes=original;bytes[at]^=1u<<bit;
    auto r=m::DecodeMetricRawSample(f.descriptor,f.series,bytes);
    if(r.ok()) {
      ++accepted;auto e=m::EncodeMetricRawSample(f.descriptor,f.series,*r.record);
      Check(e.ok()&&e.bytes==bytes,"accepted mutation normalized or lost source bytes");
    } else {++refused;Check(!r.record,"wire mutation failure exposed partial record");}
  }
  Check(accepted&&refused,"wire mutation matrix lacked accepted/refused cases");
}
void SampleRateHistogramAndLimits() {
  auto f=Sample(U(3));
  f.descriptor.type=m::MetricType::rate;f.descriptor.rate_window_nanoseconds=1000000000;
  f.descriptor.rate_source_counter_uuid=Id(40);f.descriptor.rate_source_counter_generation=41;
  static_cast<m::MetricDescriptorBinding&>(f.series)=f.descriptor;
  static_cast<m::MetricDescriptorBinding&>(f.sample)=f.descriptor;
  f.sample.value.type=m::MetricType::rate;f.sample.value.arithmetic_inexact=true;SampleRoundTrip(f);
  Check(!m::StageMetricValueUpdate(f.descriptor,{},nullptr,U(3)).ok(),"sample codec enabled raw rate production");
  f=Sample(U(3));f.descriptor.type=m::MetricType::histogram;f.descriptor.histogram_buckets={U(1),U(2)};
  auto& v=f.sample.value;v.type=m::MetricType::histogram;v.count=1;v.sum=U(3);v.bucket_bounds=f.descriptor.histogram_buckets;v.buckets={0,0,1};
  SampleRoundTrip(f);v.buckets[2]=0;SampleEncodeRejected(f);
  f=Sample(std::string(1048576-40,'v'));f.descriptor.labels.clear();f.descriptor.label_schema_uuid={};f.descriptor.label_schema_generation=0;
  f.series.labels.clear();f.sample.labels.clear();f.sample.value.labels.clear();f.series.label_schema_uuid={};f.series.label_schema_generation=0;
  f.sample.label_schema_uuid={};f.sample.label_schema_generation=0;std::get<4>(f.series.series_key)={};std::get<5>(f.series.series_key).clear();
  f.sample.clock_quality=std::string(128,'c');f.sample.freshness_class=std::string(128,'f');
  auto e=m::EncodeMetricRawSample(f.descriptor,f.series,f.sample);Check(e.ok()&&e.bytes.size()==1049112,"exact sample cap refused");
  if(e.ok())Check(m::DecodeMetricRawSample(f.descriptor,f.series,e.bytes).ok(),"maximum sample decode");
  f.sample.freshness_class.push_back('f');SampleEncodeRejected(f);
  Bytes oversized(1049113,0);SampleRejected(f,oversized);
}
void SampleFaults() {
  auto f=Sample(std::string(80,'v'));f.sample.clock_quality=std::string(80,'c');f.sample.freshness_class=std::string(80,'f');
  const auto expected=GoldenSample(f);
  for(unsigned operation=0;operation<2;++operation) {
    unsigned faults=0;bool complete=false;
    for(long point=0;point<1000;++point) {
      codec_fault::remaining=point;codec_fault::fired=false;bool ok=false,partial=false;
      if(operation==0){auto r=m::EncodeMetricRawSample(f.descriptor,f.series,f.sample);ok=r.ok();partial=!ok&&!r.bytes.empty();}
      else {auto r=m::DecodeMetricRawSample(f.descriptor,f.series,expected);ok=r.ok();partial=!ok&&r.record.has_value();}
      const auto fired=codec_fault::fired;codec_fault::remaining=-1;
      Check(!partial,"sample allocation failure exposed partial output");
      if(fired){++faults;Check(!ok,"sample allocation failure reported success");}
      else {Check(ok,"sample operation failed without allocation fault");complete=true;break;}
      Check(GoldenSample(f)==expected,"sample allocation failure mutated input");
    }
    Check(complete&&faults,"sample allocation sweep incomplete");
    std::cout<<"sample allocation operation="<<operation<<" injected="<<faults<<'\n';
  }
}
}
#ifndef METRIC_SAMPLE_CODEC_MAIN
#define METRIC_SAMPLE_CODEC_MAIN main
#endif
int METRIC_SAMPLE_CODEC_MAIN() {
  MetricValueCodecBaseMain();const auto before=checks;
  SampleTypesAndBindings();SampleRateHistogramAndLimits();SampleWireMutations();SampleFaults();
  std::cout<<"metric sample binary checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
