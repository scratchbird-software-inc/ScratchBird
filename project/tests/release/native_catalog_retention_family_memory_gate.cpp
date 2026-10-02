// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main NativeCatalogValueRegressionMain
#include "native_catalog_value_memory_gate.cpp"
#undef main
#include "catalog_metric_retention_policy.hpp"

namespace {
namespace m = scratchbird::core::metrics;
std::string_view View(const Bytes& b) {return {reinterpret_cast<const char*>(b.data()),b.size()};}
std::vector<WireField> Retention(unsigned scope,unsigned mode,unsigned overflow,const Bytes& grains,
    p::u64 raw,p::u64 rollup,std::string_view annotation="policy-\xc3\xa9") {
  const Bytes text(annotation.begin(),annotation.end());
  return {{1,T::engine_identity,Identity(1)},{2,T::unsigned_integer,Integer(1)},
      {3,T::utf8_text,text},{4,T::unsigned_integer,Integer(scope)},{5,T::unsigned_integer,Integer(mode)},
      {6,T::unsigned_integer,Integer(raw)},{7,T::unsigned_integer,Integer(rollup)},
      {8,T::opaque_bytes,grains},{9,T::unsigned_integer,Integer(1)},{10,T::unsigned_integer,Integer(1)},
      {11,T::unsigned_integer,Integer(overflow)},{12,T::utf8_text,text},{13,T::utf8_text,text},
      {14,T::boolean,{1}},{15,T::engine_identity,Identity(8)},{16,T::unsigned_integer,Integer(1)}};
}
auto Decode(const Bytes& b){return NoHeap([&]{return c::DecodeCatalogMetricRetentionPolicyView(View(b));});}
void Refuse(const Bytes& b,E error) {
  const auto v=Decode(b);Check(v.error==error&&!v.record,"borrowed retention exact refusal/no partial");
  const auto owned=c::DecodeCatalogMetricRetentionPolicy(View(b));
  Check(owned.error==error&&!owned.record,"owning retention same refusal/no partial");
}
std::vector<Bytes> GrainPermutations() {
  std::vector<Bytes> out{{}};
  for(std::size_t i=0;i<out.size();++i)for(unsigned g=0;g<4;++g){
    if(std::find(out[i].begin(),out[i].end(),g)!=out[i].end())continue;
    auto next=out[i];next.push_back(g);out.push_back(std::move(next));
  }
  Check(out.size()==65,"independent ordered grain inventory");return out;
}
c::CatalogMetadataVersion Metadata(const Bytes& b,unsigned scope) {
  c::CatalogMetadataVersion x;x.record.header.kind=c::CatalogRecordKind::policy;
  x.record.header.object_uuid={p::UuidKind::object,Id(1)};
  x.record.header.parent_uuid={p::UuidKind::object,Id(3)};x.owning_schema_uuid={p::UuidKind::schema,Id(3)};
  x.default_name_uuid={p::UuidKind::object,Id(4)};x.name_vector_uuid={p::UuidKind::object,Id(5)};
  x.creator_transaction_uuid={p::UuidKind::transaction,Id(8)};x.creator_local_transaction_id=1;
  x.definition_version=1;x.object_subtype="metric_retention";x.record.payload=View(b);
  x.authority_scope=scope==4?c::CatalogAuthorityScope::cluster:c::CatalogAuthorityScope::local;return x;
}
void Matrix() {
  const auto permutations=GrainPermutations();
  for(unsigned scope=1;scope<=4;++scope)for(unsigned mode=0;mode<3;++mode)for(unsigned overflow=1;overflow<=3;++overflow)
    for(const auto& grains:permutations)for(unsigned raw=0;raw<2;++raw)for(unsigned rollup=0;rollup<2;++rollup){
    const bool valid=mode==0?(!raw&&!rollup&&grains.empty()):mode==1?(raw&&(grains.empty()||rollup)):(!raw&&rollup&&!grains.empty());
    const auto b=Wire(Retention(scope,mode,overflow,grains,raw,rollup),65543);
    const auto decoded=Decode(b);Check(decoded.ok()==valid,"retention independent scope/mode/grain/duration matrix");
    if(!valid){Check(decoded.error==E::invalid_value&&!decoded.record,"semantic refusal no partial");continue;}
    const auto& v=*decoded.record;
    Check(v.policy_uuid==Id(1)&&v.generation==1&&v.origin_transaction_uuid.value==Id(8)&&
        v.origin_local_transaction_id==1&&v.rollup_grain_count==grains.size(),"retention native fields");
    for(std::size_t i=0;i<grains.size();++i)Check(static_cast<unsigned>(v.rollup_grains[i])==grains[i],"grain order exact");
    Check(v.policy_name.data()==reinterpret_cast<const char*>(b.data()+72),"annotation borrows exact source bytes");
    const auto copied=v;Check(copied.rollup_grains==v.rollup_grains,"copy has inline grains not self referencing span");
    const auto owned=c::DecodeCatalogMetricRetentionPolicy(View(b));Check(owned.ok(),"owning retention parity");
    Check(c::EncodeCatalogMetricRetentionPolicy(*owned.record).bytes==b,"independent exact canonical byte oracle");
    auto metadata=Metadata(b,scope);
    Check(NoHeap([&]{return c::CatalogMetricRetentionPolicyMatchesMetadata(metadata)&&
        c::CatalogMetadataPreservesFamilyOrigin(metadata,metadata);}),"shared metadata/origin no heap");
    metadata.authority_scope=scope==4?c::CatalogAuthorityScope::local:c::CatalogAuthorityScope::cluster;
    Check(!NoHeap([&]{return c::CatalogMetricRetentionPolicyMatchesMetadata(metadata);}),"wrong authority scope refused");
  }
}
void Boundaries() {
  auto f=Retention(1,1,1,{3,2,1,0},~p::u64{0},~p::u64{0},std::string(4096,'P'));
  for(unsigned i:{1u,8u,9u,15u})f[i].value=Integer(~p::u64{0});
  auto b=Wire(f,65543);const auto v=Decode(b);Check(v.ok(),"maximum annotations/counters/durations");
  const auto owned=c::DecodeCatalogMetricRetentionPolicy(View(b));Check(owned.ok(),"maximum owning result");
  std::fill(b.begin(),b.end(),0);Check(owned.record->policy.policy_name==std::string(4096,'P'),"owning annotations survive input mutation");
  for(unsigned field:{2u,8u,9u,10u,16u}){auto bad=f;bad[field-1].value=Integer(0);
    // field8 is the grain vector, not a zero counter.
    if(field==8)bad[7].value={0,0};
    Refuse(Wire(bad,65543),E::invalid_value);}
  for(unsigned field:{3u,12u,13u})for(unsigned size:{0u,4097u}){auto bad=f;bad[field-1].value.assign(size,'P');
    Refuse(Wire(bad,65543),size?E::size_limit:E::invalid_value);}
  for(unsigned field:{3u,12u,13u}){auto bad=f;bad[field-1].value={'A',0,'B'};Refuse(Wire(bad,65543),E::invalid_value);}
  for(unsigned field:{4u,5u,11u})for(auto value:{p::u64{99},~p::u64{0}}){auto bad=f;bad[field-1].value=Integer(value);Refuse(Wire(bad,65543),E::invalid_value);}
  for(unsigned field:{4u,11u}){auto bad=f;bad[field-1].value=Integer(0);Refuse(Wire(bad,65543),E::invalid_value);}
  for(const auto grains:{Bytes{4},Bytes{0,1,0},Bytes{0,1,2,3,0}}){auto bad=f;bad[7].value=grains;Refuse(Wire(bad,65543),grains.size()>4?E::size_limit:E::invalid_value);}
  for(unsigned flag:{0u,2u,255u}){auto bad=f;bad[13].value={static_cast<p::byte>(flag)};Refuse(Wire(bad,65543),E::invalid_value);}
  for(unsigned field:{1u,15u})for(unsigned version=0;version<16;++version)if(version!=7){auto bad=f;
    bad[field-1].value[6]=version<<4;Refuse(Wire(bad,65543),E::invalid_value);}
  const auto original=Wire(Retention(1,0,1,{},0,0),65543);auto a=Metadata(original,1),next=a;
  auto nf=Retention(1,0,1,{},0,0);nf[1].value=Integer(2);next.record.payload=View(Wire(nf,65543));
  next.definition_version=2;next.creator_transaction_uuid.value=Id(10);next.creator_local_transaction_id=2;
  Check(NoHeap([&]{return c::CatalogMetricRetentionPolicyPreservesOrigin(a,next);}),"successor has independent creator");
  for(unsigned index:{0u,3u,14u,15u}){auto bad=nf;bad[index].value=index==3||index==15?Integer(2):Identity(11);
    next.record.payload=View(Wire(bad,65543));Check(!NoHeap([&]{return c::CatalogMetricRetentionPolicyPreservesOrigin(a,next);}),"original identity/scope preserved");}
  for(std::size_t n=0;n<original.size();++n){const auto r=Decode(Bytes(original.begin(),original.begin()+n));Check(!r.ok()&&!r.record,"every truncation refused");}
  for(unsigned offset:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto bad=original;bad[offset]^=1;
    const auto r=Decode(bad);Check(!r.ok()&&!r.record,"malformed framing no partial");}
}
void Diagnostics() {
  m::MetricRetentionPolicyDefinition owned;owned.policy_name="policy";
  auto verify=[&](std::string_view expected){
    const m::MetricRetentionPolicyDefinitionView view{owned.policy_name,owned.scope,owned.mode,
        owned.raw_retention_seconds,owned.rollup_retention_seconds,owned.rollup_grains,
        owned.purge_batch_limit,owned.max_cardinality,owned.overflow_behavior,owned.edit_right,
        owned.default_admin_group,owned.evidence_required};
    const auto result=NoHeap([&]{return m::ValidateMetricRetentionPolicyDefinitionView(view);});
    Check(result.ok==expected.empty()&&result.detail==expected,"independent diagnostic detail and precedence");
    Check(result.diagnostic_code==(expected.empty()?"":"METRIC.RETENTION_POLICY_INVALID"),"registered diagnostic code");
    const auto legacy=m::ValidateMetricRetentionPolicyDefinition(owned);
    Check(legacy.ok==result.ok&&legacy.diagnostic_code==result.diagnostic_code&&legacy.detail==result.detail,"owning diagnostic parity");
  };
  verify("");owned.policy_name="";owned.scope="invalid";owned.purge_batch_limit=0;verify("invalid_policy_annotation");
  owned.policy_name="policy";verify("invalid_scope");owned.scope="local";verify("invalid_limits_or_missing_evidence");
  owned.purge_batch_limit=1;owned.overflow_behavior="invalid";verify("invalid_overflow_behavior");
  owned.overflow_behavior="reject_and_evidence";owned.rollup_grains={m::MetricRollupGrain::invalid};verify("invalid_rollup_grain");
  owned.rollup_grains={m::MetricRollupGrain::one_minute,m::MetricRollupGrain::one_minute};verify("duplicate_rollup_grain");
  owned.rollup_grains.resize(1);verify("current_only_has_history");owned.mode=m::MetricRetentionMode::rollup_only;verify("invalid_rollup_only_retention");
  owned.mode=m::MetricRetentionMode::raw_and_rollup;verify("invalid_raw_and_rollup_retention");owned.mode=m::MetricRetentionMode::invalid;verify("invalid_retention_mode");
}
}
int main(){try{Matrix();Boundaries();Diagnostics();Check(attempted_allocations==0,"retention borrowed route allocated");
  std::cout<<"native retention family checks="<<checks<<" attempted_allocations="<<attempted_allocations<<'\n';return 0;
}catch(...){deny_heap=false;std::cerr<<"retention family failed; attempted_allocations="<<attempted_allocations<<'\n';return 1;}}
