// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "metric_history.hpp"
#include "metric_label_key.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <set>
#include <type_traits>
#include <thread>

#if defined(SB_METRIC_HISTORY_ENTROPY_FAULT_TEST)
namespace { thread_local bool reject_entropy=false; thread_local unsigned entropy_faults=0; }
extern "C" int __real_RAND_bytes(unsigned char*,int);
extern "C" int __wrap_RAND_bytes(unsigned char* bytes,int size) {
  if(reject_entropy){++entropy_faults;return 0;}
  return __real_RAND_bytes(bytes,size);
}
#endif

namespace m = scratchbird::core::metrics;
namespace p = scratchbird::core::platform;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* why) {
  ++checks; if (!value) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
m::MetricUuid Id(unsigned char tag) { m::MetricUuid r; r.bytes[6]=0x70; r.bytes[8]=0x80; r.bytes[15]=tag; return r; }
static_assert(std::is_same_v<decltype(m::MetricSeriesIdentity::series_uuid),m::MetricUuid>);
static_assert(std::is_same_v<decltype(m::MetricRawSampleRecord::sample_uuid),m::MetricUuid>);
static_assert(std::is_same_v<decltype(m::MetricRollupRecord::rollup_uuid),m::MetricUuid>);
static_assert(std::is_same_v<decltype(m::MetricRetentionEvidenceRecord::evidence_uuid),m::MetricUuid>);
static_assert(std::is_same_v<decltype(m::MetricRetentionEvidenceRecord::actor_uuid),m::MetricUuid>);
m::MetricHistoryBinding Binding();
m::MetricDescriptor Descriptor() {
  m::MetricDescriptor d;
  static_cast<m::MetricDescriptorBinding&>(d)=Binding();
  d.value_type=m::MetricScalarType::float64;
  d.family="record-test"; d.namespace_path="sys.metrics.test"; d.producer_owner="test-owner";
  d.labels={{"object",true,true,m::MetricLabelType::system_uuid},
            {"data",true,false,m::MetricLabelType::uuid_value},
            {"text",false,false,m::MetricLabelType::text}};
  return d;
}
m::MetricHistoryBinding Binding() {
  m::MetricHistoryBinding b;
  b.metric_uuid=Id(1); b.descriptor_generation=11;
  b.label_schema_uuid=Id(2); b.label_schema_generation=12;
  b.retention_policy_uuid=Id(3); b.retention_policy_generation=13;
  b.visibility_policy_uuid=Id(4); b.visibility_policy_generation=14;
  b.database_uuid=Id(5); b.node_uuid=Id(6); return b;
}
m::MetricRetentionPolicy Policy() {
  m::MetricRetentionPolicy p; p.policy_uuid=Id(3); p.generation=13; p.policy_name="test-policy"; return p;
}
m::MetricLabelSet Labels() {
  auto data=Id(8); data.bytes[6]=0x40;
  return {{"object",Id(7)},{"data",data},{"text",std::string("a=b;|c")}};
}
template<class R> void Rejected(const R& r) { Check(!r.ok()&&!r.record,"failure exposed partial history record"); }
void Bindings() {
  const auto d=Descriptor(); const auto b=Binding(); const auto p=Policy(); const auto labels=Labels();
  const auto result=m::MakeMetricSeriesIdentity(d,labels,p,b,Id(9));
  Check(result.ok(),"valid catalog-bound series rejected");
  if(!result.ok())return;
  Check(result.record->series_uuid==Id(9)&&result.record->metric_uuid==b.metric_uuid&&
        result.record->label_schema_generation==12&&result.record->retention_policy_generation==13&&
        result.record->visibility_policy_generation==14&&result.record->redaction_class=="contains_sensitive_labels",
        "series lost supplied binary identity or generation");
  auto reordered=labels;std::reverse(reordered.begin(),reordered.end());
  auto same=m::MakeMetricSeriesIdentity(d,reordered,p,b,Id(9));
  Check(same.ok()&&same.record->series_key==result.record->series_key,"label ordering changed typed identity");
  auto changed_policy=p;changed_policy.generation++;auto changed_binding=b;changed_binding.retention_policy_generation++;
  auto changed_descriptor=d;changed_descriptor.retention_policy_generation++;
  same=m::MakeMetricSeriesIdentity(changed_descriptor,labels,changed_policy,changed_binding,Id(9));
  Check(same.ok()&&same.record->series_key==result.record->series_key,"policy version replaced series key");
  for(auto member:std::initializer_list<m::MetricUuid m::MetricHistoryBinding::*>{&m::MetricHistoryBinding::metric_uuid,&m::MetricHistoryBinding::label_schema_uuid,
      &m::MetricHistoryBinding::retention_policy_uuid,&m::MetricHistoryBinding::visibility_policy_uuid,
      &m::MetricHistoryBinding::database_uuid,&m::MetricHistoryBinding::node_uuid}) {
    auto bad=b;bad.*member={};Rejected(m::MakeMetricSeriesIdentity(d,labels,p,bad,Id(9)));
    for(unsigned version=0;version<16;++version)if(version!=7){bad=b;(bad.*member).bytes[6]=version<<4;Rejected(m::MakeMetricSeriesIdentity(d,labels,p,bad,Id(9)));}
    for(unsigned variant:{0u,0x40u,0xc0u}){bad=b;(bad.*member).bytes[8]=variant;Rejected(m::MakeMetricSeriesIdentity(d,labels,p,bad,Id(9)));}
  }
  for(auto member:{&m::MetricHistoryBinding::descriptor_generation,&m::MetricHistoryBinding::label_schema_generation,
      &m::MetricHistoryBinding::retention_policy_generation,&m::MetricHistoryBinding::visibility_policy_generation}) {
    auto bad=b;bad.*member=0;Rejected(m::MakeMetricSeriesIdentity(d,labels,p,bad,Id(9)));
  }
  changed_binding=b;changed_binding.retention_policy_uuid=Id(10);Rejected(m::MakeMetricSeriesIdentity(d,labels,p,changed_binding,Id(9)));
  for(auto member:{&m::MetricDescriptorBinding::metric_uuid,&m::MetricDescriptorBinding::label_schema_uuid,
      &m::MetricDescriptorBinding::retention_policy_uuid,&m::MetricDescriptorBinding::visibility_policy_uuid}) {
    auto wrong_descriptor=d;(wrong_descriptor.*member).bytes[15]++;
    Rejected(m::MakeMetricSeriesIdentity(wrong_descriptor,labels,p,b,Id(9)));
  }
  for(auto member:{&m::MetricDescriptorBinding::descriptor_generation,&m::MetricDescriptorBinding::label_schema_generation,
      &m::MetricDescriptorBinding::retention_policy_generation,&m::MetricDescriptorBinding::visibility_policy_generation}) {
    auto wrong_descriptor=d;wrong_descriptor.*member+=1;
    Rejected(m::MakeMetricSeriesIdentity(wrong_descriptor,labels,p,b,Id(9)));
  }
  changed_binding=b;changed_binding.cluster_uuid=Id(10);Rejected(m::MakeMetricSeriesIdentity(d,labels,p,changed_binding,Id(9)));
  auto cluster=d;cluster.cluster_only=true;changed_policy=p;changed_policy.scope="cluster";
  Rejected(m::MakeMetricSeriesIdentity(cluster,labels,changed_policy,b,Id(9)));
  Check(m::MakeMetricSeriesIdentity(cluster,labels,changed_policy,changed_binding,Id(9)).ok(),"valid cluster record construction rejected");
  Rejected(m::MakeMetricSeriesIdentity(cluster,labels,p,changed_binding,Id(9)));
  auto no_labels=d;no_labels.labels.clear();auto absent=b;absent.label_schema_uuid={};absent.label_schema_generation=0;
  no_labels.label_schema_uuid={};no_labels.label_schema_generation=0;
  Check(m::MakeMetricSeriesIdentity(no_labels,{},p,absent,Id(9)).ok(),"exact optional label-schema absence rejected");
  absent.label_schema_uuid=Id(2);Rejected(m::MakeMetricSeriesIdentity(no_labels,{},p,absent,Id(9)));
  absent=b;absent.label_schema_uuid={};Rejected(m::MakeMetricSeriesIdentity(no_labels,{},p,absent,Id(9)));
  for(unsigned version=1;version<=7;++version){auto data=labels;std::get<m::MetricUuid>(data[1].value).bytes[6]=version<<4;
    const auto r=m::MakeMetricSeriesIdentity(d,data,p,b,Id(9));Check(r.ok()&&std::get<m::MetricUuid>(r.record->labels[1].value).bytes[6]==version<<4,"user UUID value coerced or rejected");}
  auto badlabels=labels;badlabels[0].value=std::string("00000000-0000-7000-8000-000000000007");Rejected(m::MakeMetricSeriesIdentity(d,badlabels,p,b,Id(9)));
  badlabels=labels;badlabels.push_back(labels[0]);Rejected(m::MakeMetricSeriesIdentity(d,badlabels,p,b,Id(9)));
  badlabels=labels;badlabels.pop_back();const auto short_key=m::MakeMetricSeriesIdentity(d,badlabels,p,b,Id(9));
  Check(short_key.ok()&&short_key.record->series_key!=result.record->series_key,"typed label sets collided");
  Rejected(m::MakeMetricSeriesIdentity(d,labels,p,b,{}));
}
void Samples() {
  const auto d=Descriptor();const auto series=m::MakeMetricSeriesIdentity(d,Labels(),Policy(),Binding(),Id(9));
  Check(series.ok(),"sample series setup");if(!series.ok())return;
  m::MetricValue value;value.family=d.family;value.labels=Labels();value.type=d.type;value.value=13.0;
  std::set<m::MetricUuid> issued;
  for(unsigned n=0;n<1000;++n){const auto r=m::MakeMetricRawSampleRecord(d,*series.record,value,1000,1001,1);
    Check(r.ok(),"runtime UUID issuance failed");if(!r.ok())continue;
    Check(m::MetricSystemUuidValid(r.record->sample_uuid)&&issued.insert(r.record->sample_uuid).second&&r.record->sample_uuid!=series.record->series_uuid,"identical observations reused sample identity");
    Check(r.record->publication_time_utc_ns==0&&r.record->revision==1&&r.record->sample_time_utc_ns==1000&&r.record->collection_time_utc_ns==1001&&
          r.record->clock_quality.empty()&&r.record->freshness_class.empty()&&r.record->evidence_uuid.is_nil(),"construction invented publication/clock/quality/evidence");
    Check(r.record->metric_uuid==Binding().metric_uuid&&r.record->visibility_policy_generation==14&&r.record->source_sequence==1,"sample lost actual binding or sequence");
  }
  for(unsigned bad=0;bad<3;++bad)Rejected(m::MakeMetricRawSampleRecord(d,*series.record,value,bad==0?0:1,bad==1?0:2,bad==2?0:1));
  auto bad_series=*series.record;bad_series.series_uuid={};Rejected(m::MakeMetricRawSampleRecord(d,bad_series,value,1,2,1));
  bad_series=*series.record;bad_series.metric_uuid=Id(20);Rejected(m::MakeMetricRawSampleRecord(d,bad_series,value,1,2,1));
  auto bad_value=value;bad_value.family="other";Rejected(m::MakeMetricRawSampleRecord(d,*series.record,bad_value,1,2,1));
  bad_value=value;bad_value.type=m::MetricType::gauge;Rejected(m::MakeMetricRawSampleRecord(d,*series.record,bad_value,1,2,1));
  bad_value=value;bad_value.labels[0].value=Id(21);Rejected(m::MakeMetricRawSampleRecord(d,*series.record,bad_value,1,2,1));
  const auto max=std::numeric_limits<p::u64>::max();const auto r=m::MakeMetricRawSampleRecord(d,*series.record,value,max,max,max);
  Check(r.ok()&&r.record->sample_time_utc_ns==max&&r.record->source_sequence==max,"exact uint64 observation metadata narrowed");
}
void Evidence() {
  m::MetricRetentionEvidenceRecord e;
  e.policy_uuid=Id(1);e.actor_uuid=Id(2);e.transaction_uuid=Id(3);e.operation="raw_cleanup";e.decision="rejected";
  const auto a=m::MakeMetricRetentionEvidenceRecord(e),b=m::MakeMetricRetentionEvidenceRecord(e);
  Check(a.ok()&&b.ok()&&a.record->evidence_uuid!=b.record->evidence_uuid&&a.record->decision=="rejected"&&a.record->rows_affected==0,"evidence construction invented outcome or identity");
  for(auto member:{&m::MetricRetentionEvidenceRecord::policy_uuid,&m::MetricRetentionEvidenceRecord::actor_uuid,&m::MetricRetentionEvidenceRecord::transaction_uuid}) {
    auto bad=e;bad.*member={};Rejected(m::MakeMetricRetentionEvidenceRecord(bad));
    bad=e;(bad.*member).bytes[6]=0x40;Rejected(m::MakeMetricRetentionEvidenceRecord(bad));
  }
  auto bad=e;bad.evidence_uuid=Id(4);Rejected(m::MakeMetricRetentionEvidenceRecord(bad));
  bad=e;bad.series_uuid=Id(5);bad.series_uuid.bytes[6]=0x40;Rejected(m::MakeMetricRetentionEvidenceRecord(bad));
  bad=e;bad.decision.clear();Rejected(m::MakeMetricRetentionEvidenceRecord(bad));
  bad=e;bad.operation=std::string("a\0b",3);Rejected(m::MakeMetricRetentionEvidenceRecord(bad));
}
void IssuanceFailure() {
#if defined(SB_METRIC_HISTORY_ENTROPY_FAULT_TEST)
  const auto d=Descriptor();const auto series=m::MakeMetricSeriesIdentity(d,Labels(),Policy(),Binding(),Id(9));
  Check(series.ok(),"issuance fault series setup");if(!series.ok())return;
  m::MetricValue value;value.family=d.family;value.labels=Labels();value.type=d.type;value.value=0.0;
  m::MetricRetentionEvidenceRecord e;e.policy_uuid=Id(1);e.actor_uuid=Id(2);e.transaction_uuid=Id(3);e.operation="raw_cleanup";e.decision="rejected";
  for(unsigned kind=0;kind<2;++kind){bool refused=false,recovered=false;unsigned consumed=0;
    std::thread thread([&]{reject_entropy=true;
      if(kind==0){
        auto invalid=value;invalid.value=std::numeric_limits<double>::quiet_NaN();
        const auto r=m::MakeMetricRawSampleRecord(d,*series.record,invalid,1,2,1);
        Check(!r.ok()&&!r.record&&r.error==m::MetricHistoryRecordError::invalid_observation&&entropy_faults==0,
              "invalid scalar reached runtime identity issuer");
      }
      if(kind==0){const auto r=m::MakeMetricRawSampleRecord(d,*series.record,value,1,2,1);refused=!r.ok()&&!r.record&&r.error==m::MetricHistoryRecordError::identity_issuance_failed;}
      else {const auto r=m::MakeMetricRetentionEvidenceRecord(e);refused=!r.ok()&&!r.record&&r.error==m::MetricHistoryRecordError::identity_issuance_failed;}
      consumed=entropy_faults;reject_entropy=false;
      recovered=kind==0?m::MakeMetricRawSampleRecord(d,*series.record,value,1,2,1).ok():m::MakeMetricRetentionEvidenceRecord(e).ok();
    });thread.join();
    Check(consumed>0&&refused,"real entropy failure issued partial/false identity");
    Check(recovered,"identity issuer did not recover after entropy fault");
  }
#endif
}
}  // namespace
int main() {
  Bindings();Samples();Evidence();IssuanceFailure();
  std::cout<<"metric history record identity checks="<<checks<<" failures="<<failures<<'\n';
  return failures?1:0;
}
