// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse the independent scalar byte/value oracles and run their admission
// regression too; production update code is compiled separately by CMake.
#define main MetricScalarAdmissionRegressionMain
#include "metric_scalar_test.cpp"
#undef main
#include "metric_value_update.hpp"
#include "metric_label_key.hpp"
#include "canonical_diagnostic_catalog.hpp"
#include <mpfr.h>
#include <cfenv>
#include <cstring>
#include <cstdlib>
#include <new>

namespace allocation_fault {
thread_local long countdown=-1;
thread_local unsigned faults=0;
void* Allocate(std::size_t size){
  if(countdown==0){++faults;throw std::bad_alloc();}
  if(countdown>0)--countdown;
  if(void* p=std::malloc(size?size:1))return p;
  throw std::bad_alloc();
}
}
void* operator new(std::size_t n){return allocation_fault::Allocate(n);}
void* operator new[](std::size_t n){return allocation_fault::Allocate(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,const std::nothrow_t&) noexcept{try{return allocation_fault::Allocate(n);}catch(...){return nullptr;}}
void* operator new[](std::size_t n,const std::nothrow_t&) noexcept{try{return allocation_fault::Allocate(n);}catch(...){return nullptr;}}
void operator delete(void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept{std::free(p);}

namespace {
using UE=m::MetricValueUpdateError;
bool Same(const m::MetricValue& a,const m::MetricValue& b){
  return a.family==b.family&&a.type==b.type&&a.value==b.value&&a.sum==b.sum&&a.count==b.count&&
      a.buckets==b.buckets&&a.bucket_bounds==b.bucket_bounds&&a.buckets_cumulative==b.buckets_cumulative&&
      a.arithmetic_inexact==b.arithmetic_inexact&&a.state_text==b.state_text&&
      m::MakeMetricSeriesKey(a.family,a.labels)==m::MakeMetricSeriesKey(b.family,b.labels);
}
void Sum(const m::MetricScalar& a,const m::MetricScalar& b,const m::MetricScalar& expected,bool inexact=false){
  const auto result=m::AddMetricScalars(a,b);
  Check(result.ok()&&result.value==expected&&result.inexact==inexact,"checked exact/rounded sum");
  const auto reverse=m::AddMetricScalars(b,a);
  Check(reverse.ok()&&reverse.value==result.value&&reverse.inexact==result.inexact,"addition commutativity");
}
void Overflow(const m::MetricScalar& a,const m::MetricScalar& b){
  const auto result=m::AddMetricScalars(a,b);Check(!result.ok()&&!result.value&&result.error==E::overflow,"arithmetic overflow exposed partial/wrapped value");
}
void Arithmetic(){
  Sum(U(9007199254740992ULL),U(1),U(9007199254740993ULL));Sum(U(-2),U(1),U(-1));Overflow(U(-1),U(1));
  Sum(std::numeric_limits<I>::min(),I(1),std::numeric_limits<I>::min()+I(1));
  Overflow(std::numeric_limits<I>::min(),I(-1));Overflow(std::numeric_limits<I>::max(),I(1));
  Sum(std::numeric_limits<I>::min(),std::numeric_limits<I>::max(),I(-1));
  Sum(1.,2.,3.);Sum(9007199254740992.,1.,9007199254740992.,true);
  Sum(9007199254740994.,1.,9007199254740996.,true);
  Sum(std::numeric_limits<double>::denorm_min(),-std::numeric_limits<double>::denorm_min(),0.);
  Sum(std::numeric_limits<double>::min(),-std::nextafter(std::numeric_limits<double>::min(),0.),std::numeric_limits<double>::denorm_min());
  Overflow(std::numeric_limits<double>::max(),std::numeric_limits<double>::max());
  Sum(Binary("1"),Binary("2"),Binary("3"));
  Sum(Binary("10384593717069655257060992658440192"),Binary("1"),Binary("10384593717069655257060992658440192"),true);
  Overflow(Binary("1e4932"),Binary("1e4932"));
  Sum(Decimal("1"),Decimal("2"),Decimal("3"));Sum(Decimal("100",-2),Decimal("1"),Decimal("200",-2));
  Sum(Decimal("9999999999999999999999999999999999"),Decimal("1"),Decimal("1000000000000000000000000000000000",1));
  Sum(Decimal("1000000000000000000000000000000000",1),Decimal("5"),Decimal("1000000000000000000000000000000000",1),true);
  Sum(Decimal("1000000000000000000000000000000001",1),Decimal("5"),Decimal("1000000000000000000000000000000002",1),true);
  Sum(Decimal("1",6111),Decimal("1",-6176),Decimal("1000000000000000000000000000000000",6078),true);
  Sum(Decimal("1",-6176),Decimal("1",-6176,true),Decimal("0",-6176));
  Sum(Decimal("12",-1,true),Decimal("3",-1),Decimal("9",-1,true));
  Sum(Decimal("1000000000000000000000000000000001",1,true),Decimal("5",0,true),Decimal("1000000000000000000000000000000002",1,true),true);
  Overflow(Decimal("9999999999999999999999999999999999",6111),Decimal("9999999999999999999999999999999999",6111));
  for(const auto& pair:std::vector<std::pair<m::MetricScalar,m::MetricScalar>>{{-0.,-0.},{Binary("-0"),Binary("-0")},{Decimal("0",0,true),Decimal("0",0,true)}}){
    const auto result=m::AddMetricScalars(pair.first,pair.second);Check(result.ok()&&result.value==pair.first,"negative zero arithmetic");
    if(result.ok()&&std::holds_alternative<double>(*result.value))Check(std::signbit(std::get<double>(*result.value)),"binary64 negative zero sign");
  }
  for(const auto& bad:std::vector<m::MetricScalar>{std::monostate{},true,std::string("5"),Id(1),m::MetricEnumValue{1}}){
    const auto r=m::AddMetricScalars(bad,bad);Check(!r.ok()&&!r.value,"nonnumeric arithmetic admitted");}
  Check(m::AddMetricScalars(U(1),1.).error==E::type_mismatch,"mixed numeric arithmetic admitted");
  U rng=0x817364;
  for(unsigned n=0;n<200;++n){
    rng=rng*6364136223846793005ULL+1;const U ac=rng;const int ae=int((rng>>32)%13)-6;const bool an=rng>>63;
    rng=rng*6364136223846793005ULL+1;const U bc=rng;const int be=int((rng>>32)%13)-6;const bool bn=rng>>63;
    const int common=std::min(ae,be);Big av=ac,bv=bc;
    for(int i=common;i<ae;++i)av*=10;for(int i=common;i<be;++i)bv*=10;
    if(an)av=-av;if(bn)bv=-bv;Big exact=av+bv;const bool negative=exact<0;if(negative)exact=-exact;
    Sum(Decimal(std::to_string(ac),ae,an),Decimal(std::to_string(bc),be,bn),Decimal(exact.convert_to<std::string>(),common,negative));
  }
}
void Binary64Oracle(){
  const int original=std::fegetround();
  mpfr_t a,b,exact;mpfr_inits2(2200,a,b,exact,(mpfr_ptr)nullptr);
  U rng=0x837497302;
  for(unsigned n=0;n<600;++n){
    rng=rng*6364136223846793005ULL+1;U ab=rng;if(((ab>>52)&2047)==2047)ab^=U(1)<<52;
    rng=rng*6364136223846793005ULL+1;U bb=rng;if(((bb>>52)&2047)==2047)bb^=U(1)<<52;
    double av=0,bv=0;std::memcpy(&av,&ab,8);std::memcpy(&bv,&bb,8);
    mpfr_set_d(a,av,MPFR_RNDN);mpfr_set_d(b,bv,MPFR_RNDN);mpfr_add(exact,a,b,MPFR_RNDN);
    const double expected=mpfr_get_d(exact,MPFR_RNDN);const bool inexact=mpfr_cmp_d(exact,expected)!=0;
    for(int rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){
      Check(std::fesetround(rounding)==0,"set caller rounding mode");const auto r=m::AddMetricScalars(av,bv);
      if(std::isfinite(expected))Check(r.ok()&&std::get<double>(*r.value)==expected&&r.inexact==inexact,"binary64 differs from exact2200-bit MPFR oracle");
      else Check(!r.ok()&&!r.value&&r.error==E::overflow,"oracle overflow not reported");
      Check(std::fegetround()==rounding,"arithmetic changed caller rounding mode");
    }
  }
  Check(std::fesetround(original)==0,"restore caller rounding mode");mpfr_clears(a,b,exact,(mpfr_ptr)nullptr);
}
void Updates(){
  auto noncanonical=Descriptor(T::float64);noncanonical.type=m::MetricType::derived;
  Check(!m::ValidateMetricValueDescriptor(noncanonical),"legacy derived class substituted for source-bound rate");
  auto d=Descriptor(T::uint64);d.type=m::MetricType::counter;d.min_value=U(100);
  auto r=m::StageMetricValueUpdate(d,{},nullptr,U(100));Check(r.ok(),"counter first update");if(!r.ok())return;
  const auto previous=*r.value,before=previous;r=m::StageMetricValueUpdate(d,{},&previous,U(1));
  Check(r.ok()&&r.value->value==m::MetricScalar(U(101))&&Same(previous,before),"counter delta incorrectly checked against absolute minimum");
  d.min_value.reset();auto full=previous;full.value=U(-1);const auto saved=full;
  r=m::StageMetricValueUpdate(d,{},&full,U(1));Check(!r.ok()&&!r.value&&r.error==UE::overflow&&Same(full,saved),"counter overflow mutated current state");
  auto signed_d=Descriptor(T::int64);signed_d.type=m::MetricType::counter;
  r=m::StageMetricValueUpdate(signed_d,{},nullptr,I(-1));Check(!r.ok()&&!r.value&&r.error==UE::negative_delta,"negative counter delta");
  d=Descriptor(T::float64);d.type=m::MetricType::counter;r=m::StageMetricValueUpdate(d,{},nullptr,9007199254740992.);
  auto rounded=m::StageMetricValueUpdate(d,{},&*r.value,1.);Check(rounded.ok()&&rounded.value->arithmetic_inexact,"counter rounding fact missing");
  r=m::StageMetricValueUpdate(d,{},&*rounded.value,2.);Check(r.ok()&&r.value->arithmetic_inexact,"counter rounding fact lost");
  d=Descriptor(T::float64);d.type=m::MetricType::histogram;d.histogram_buckets={1e20};
  r=m::StageMetricValueUpdate(d,{},nullptr,1e16);rounded=m::StageMetricValueUpdate(d,{},&*r.value,1.);
  Check(rounded.ok()&&rounded.value->arithmetic_inexact,"histogram rounding fact missing");
  r=m::StageMetricValueUpdate(d,{},&*rounded.value,0.);Check(r.ok()&&r.value->arithmetic_inexact,"histogram rounding fact lost");
  d=Descriptor(T::enumeration);d.type=m::MetricType::state;d.enum_values={1,7};
  r=m::StageMetricValueUpdate(d,{},nullptr,m::MetricEnumValue{7},"display only");Check(r.ok()&&r.value->state_text=="display only","typed enum state update");
  auto bad=m::StageMetricValueUpdate(d,{},&*r.value,m::MetricEnumValue{8},"display only");Check(!bad.ok()&&!bad.value,"display annotation authorized unknown enum");
  for(bool cumulative:{true,false}){
    d=Descriptor(T::uint64);d.type=m::MetricType::histogram;d.histogram_buckets={U(10),U(20)};d.histogram_cumulative=cumulative;
    std::optional<m::MetricValue> current;
    for(U observation:{U(5),U(15),U(25),U(10)}){r=m::StageMetricValueUpdate(d,{},current?&*current:nullptr,observation);Check(r.ok(),"histogram update");if(r.ok())current=*r.value;}
    Check(current&&current->value==m::MetricScalar(U(10))&&current->sum==m::MetricScalar(U(55))&&current->count==4&&
          current->buckets==(cumulative?std::vector<U>{2,3,4}:std::vector<U>{2,1,1}),"typed histogram sum/bucket/terminal semantics");
    if(!current)continue;
    m::MetricHistoryBinding binding;binding.metric_uuid=Id(1);binding.descriptor_generation=1;
    binding.retention_policy_uuid=Id(2);binding.retention_policy_generation=1;
    binding.visibility_policy_uuid=Id(3);binding.visibility_policy_generation=1;binding.database_uuid=Id(4);binding.node_uuid=Id(5);
    m::MetricRetentionPolicy policy;policy.policy_uuid=Id(2);policy.generation=1;policy.policy_name="typed-histogram";
    static_cast<m::MetricDescriptorBinding&>(d)=binding;
    const auto series=m::MakeMetricSeriesIdentity(d,{},policy,binding,Id(6));Check(series.ok(),"histogram history binding");
    if(series.ok()){
      const auto sample=m::MakeMetricRawSampleRecord(d,*series.record,*current,1,2,1);
      Check(sample.ok()&&Same(sample.record->value,*current),"real history factory lost typed histogram state");
      auto malformed=*current;malformed.buckets.back()=U(-1);
      const auto refused=m::MakeMetricRawSampleRecord(d,*series.record,malformed,1,2,1);
      Check(!refused.ok()&&!refused.record,"history factory issued malformed histogram occurrence");
    }
    const auto untouched=*current;auto broken=*current;broken.buckets.pop_back();
    r=m::StageMetricValueUpdate(d,{},&broken,U(1));Check(!r.ok()&&!r.value&&r.error==UE::invalid_current,"malformed current bucket shape accepted");
    broken=*current;broken.count=U(-1);broken.buckets=cumulative?std::vector<U>{0,0,U(-1)}:std::vector<U>{0,0,U(-1)};
    r=m::StageMetricValueUpdate(d,{},&broken,U(1));Check(!r.ok()&&!r.value&&r.error==UE::overflow,"histogram count wrapped");
    broken=*current;broken.sum=U(-1);r=m::StageMetricValueUpdate(d,{},&broken,U(1));
    Check(!r.ok()&&!r.value&&r.error==UE::overflow&&Same(*current,untouched),"histogram sum overflow published partial buckets");
    auto wrong=d;wrong.histogram_buckets={U(20),U(10)};Check(!m::ValidateMetricValueDescriptor(wrong),"unsorted bounds accepted");
    wrong=d;wrong.histogram_buckets={U(10),U(10)};Check(!m::ValidateMetricValueDescriptor(wrong),"duplicate bounds accepted");
    wrong=d;wrong.histogram_buckets={10.,20.};Check(!m::ValidateMetricValueDescriptor(wrong),"mixed-type bounds accepted");
    wrong=d;wrong.histogram_buckets.clear();Check(!m::ValidateMetricValueDescriptor(wrong),"empty bounds accepted");
    wrong=d;wrong.histogram_buckets={U(11),U(20)};
    r=m::StageMetricValueUpdate(wrong,{},&*current,U(1));Check(!r.ok()&&!r.value&&r.error==UE::invalid_current,"changed descriptor reinterpreted existing buckets");
    wrong=d;wrong.histogram_cumulative=!cumulative;
    r=m::StageMetricValueUpdate(wrong,{},&*current,U(1));Check(!r.ok()&&!r.value&&r.error==UE::invalid_current,"changed bucket mode reinterpreted existing counts");
  }
}
template<class Value> void CopyFailure(const Value& source,Value target){
  const auto saved=target;bool refused=false;allocation_fault::faults=0;allocation_fault::countdown=0;
  try {Value copied(source);refused=copied!=source;}catch(const std::bad_alloc&){refused=true;}
  const auto constructor_faults=allocation_fault::faults;allocation_fault::countdown=-1;
  Check(refused&&constructor_faults==1,"throwing carrier copy construction did not unwind safely");
  refused=false;allocation_fault::faults=0;allocation_fault::countdown=0;
  try {target=source;}catch(const std::bad_alloc&){refused=true;}
  const auto assignment_faults=allocation_fault::faults;allocation_fault::countdown=-1;
  Check(refused&&assignment_faults==1&&target==saved,"throwing carrier assignment changed target");
}
void AllocationFailure(){
  CopyFailure(m::MetricLabelValue(std::string(150,'l')),m::MetricLabelValue(Id(1)));
  CopyFailure(m::MetricScalar(std::string(150,'s')),m::MetricScalar(U(-1)));
  auto d=Descriptor(T::uint64);d.family=std::string(150,'f');d.type=m::MetricType::histogram;d.histogram_buckets={U(10),U(20)};
  d.labels={{"tag",true,false,m::MetricLabelType::text}};m::MetricLabelSet labels={{"tag",std::string(150,'a')}};
  auto first=m::StageMetricValueUpdate(d,labels,nullptr,U(5));Check(first.ok(),"allocation fixture initialization");if(!first.ok())return;
  const auto previous=*first.value,saved=previous;unsigned exercised=0;bool reached_success=false;
  for(long site=0;site<256;++site){allocation_fault::countdown=site;allocation_fault::faults=0;
    const auto r=m::StageMetricValueUpdate(d,labels,&previous,U(15));const auto faults=allocation_fault::faults;allocation_fault::countdown=-1;
    if(!faults){Check(r.ok(),"unfaulted stage failed");reached_success=true;break;}
    ++exercised;Check(!r.ok()&&!r.value&&r.error==UE::allocation_failure&&Same(previous,saved),"allocation fault published partial metric state");
  }
  Check(reached_success&&exercised>=4,"allocation sites not fully traversed");
  const m::MetricScalar large=Decimal("1",100),small=Decimal("1");bool arithmetic_failed=false;
  allocation_fault::countdown=0;allocation_fault::faults=0;const auto sum=m::AddMetricScalars(large,small);arithmetic_failed=allocation_fault::faults>0;allocation_fault::countdown=-1;
  Check(arithmetic_failed&&!sum.ok()&&!sum.value&&sum.error==E::allocation_failure,"decimal allocation failure not explicit");
  std::cout<<"metric update allocation sites="<<exercised<<'\n';
}
void Diagnostics(){
  namespace diag=scratchbird::core::diagnostics;
  for(const auto& entry:std::vector<std::pair<const char*,const char*>>{
      {"METRIC.VALUE_INVALID","22023"},{"METRIC.CURRENT_VALUE_INVALID","XX001"},
      {"METRIC.AGGREGATE_OVERFLOW","22003"},{"METRIC.OBSERVATION_RESOURCE_EXHAUSTED","53200"},
      {"METRIC.ARITHMETIC_FAILED","XX000"}}){
    const auto* found=diag::FindCanonicalDiagnosticCode(entry.first);
    Check(found&&found->sqlstate==entry.second&&found->is_failure,"exact metric diagnostic registration");
  }
}
}
int main(){
  MetricScalarAdmissionRegressionMain();Arithmetic();Binary64Oracle();Updates();AllocationFailure();Diagnostics();
  std::cout<<"metric typed update checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
