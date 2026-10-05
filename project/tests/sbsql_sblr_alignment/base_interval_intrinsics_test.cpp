// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_interval.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;
namespace {

unsigned checks;
unsigned vectors;
void Check(bool value,std::string_view message){
  ++checks;
  if(!value){std::cerr<<"FAIL "<<message<<'\n';std::exit(EXIT_FAILURE);}
}
p::Uuid D710(){return p::Uuid(std::array<p::byte,16>{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x10});}
std::shared_ptr<const dt::IntervalValidatedProfileHandleV3> Profile(){auto x=dt::BuildCurrentIntervalValidatedProfileHandleV3(D710());Check(x.ok(),"profile");return std::make_shared<const dt::IntervalValidatedProfileHandleV3>(std::move(x.profile));}
bool Cancel(void*) noexcept{return true;}

template<class R> bool Diagnostic(const R&r,std::string_view code){return !r.ok()&&r.diagnostic.diagnostic_code==code;}
bool Default(const dt::IntervalValueResultV3&r){return !r.value.profile&&r.value.state==dt::IntervalValueStateV3::value&&r.value.months==0&&r.value.civil_days==0&&r.value.fixed_nanoseconds==0;}
bool Default(const dt::IntervalViewResultV3&r){return !r.value.profile&&r.value.state==dt::IntervalValueStateV3::value&&r.value.months==0&&r.value.civil_days==0&&r.value.fixed_nanoseconds==0;}
bool Default(const dt::IntervalComponentResultV3&r){return !r.is_null&&r.component.months==0&&r.component.civil_days==0&&r.component.fixed_nanoseconds==0;}

enum class Fault { none, wrong_receipt, wrong_profile, dirty_null, resource, cancel };
struct Observation {bool ok=false;std::string_view diagnostic;bool default_output=true;bool is_null=false;std::int32_t months=0,civil_days=0;std::int64_t nanoseconds=0;};

std::shared_ptr<const dt::IntervalValidatedProfileHandleV3> FaultProfile(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile,
    Fault fault){
  if(fault!=Fault::wrong_receipt&&fault!=Fault::wrong_profile)return profile;
  auto changed=std::make_shared<dt::IntervalValidatedProfileHandleV3>(*profile);
  if(fault==Fault::wrong_receipt)--changed->receipt.catalog_generation;
  else changed->profile_fingerprint[0]^=1;
  return changed;
}

Observation Run(dt::IntervalIntrinsicOperationV3 op,
                const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& base,
                dt::IntervalValueStateV3 state,Fault fault){
  const auto profile=FaultProfile(base,fault);
  dt::IntervalExecutionControlV3 control;
  if(fault==Fault::resource)control.maximum_allocation_bytes=15;
  if(fault==Fault::cancel)control.cancelled=Cancel;
  const bool dirty=fault==Fault::dirty_null;
  const std::int32_t m=1,d=-2;
  const std::int64_t n=3;
  dt::IntervalOwnedValueV3 value{profile,state,m,d,n};
  if(state==dt::IntervalValueStateV3::sql_null&&!dirty)value.months=value.civil_days=0,value.fixed_nanoseconds=0;
  Observation out;
  switch(op){
    case dt::IntervalIntrinsicOperationV3::validate:{
      auto r=dt::ValidateIntervalValueViewV3(value.view(),true,control);out={r.ok(),r.diagnostic.diagnostic_code,Default(r),r.value.state==dt::IntervalValueStateV3::sql_null,r.value.months,r.value.civil_days,r.value.fixed_nanoseconds};break;}
    case dt::IntervalIntrinsicOperationV3::canonicalize:{
      auto r=dt::CanonicalizeIntervalIdentityV3(value,true,control);out={r.ok(),r.diagnostic.diagnostic_code,Default(r),r.value.state==dt::IntervalValueStateV3::sql_null,r.value.months,r.value.civil_days,r.value.fixed_nanoseconds};break;}
    case dt::IntervalIntrinsicOperationV3::construct:{
      dt::IntervalNullableI32FactV3 fm,fd;dt::IntervalNullableI64FactV3 fn;fm.state=fd.state=fn.state=state;fm.value=m;fd.value=d;fn.value=n;if(state==dt::IntervalValueStateV3::sql_null&&!dirty)fm.value=fd.value=fn.value=0;
      auto r=dt::ConstructIntervalV3(profile,fm,fd,fn,true,control);out={r.ok(),r.diagnostic.diagnostic_code,Default(r),r.value.state==dt::IntervalValueStateV3::sql_null,r.value.months,r.value.civil_days,r.value.fixed_nanoseconds};break;}
    case dt::IntervalIntrinsicOperationV3::decompose:{
      auto r=dt::DecomposeIntervalV3(value.view(),true,control);out={r.ok(),r.diagnostic.diagnostic_code,Default(r),r.is_null,r.component.months,r.component.civil_days,r.component.fixed_nanoseconds};break;}
    case dt::IntervalIntrinsicOperationV3::negate:{
      auto r=dt::NegateIntervalCheckedV3(value,true,control);out={r.ok(),r.diagnostic.diagnostic_code,Default(r),r.value.state==dt::IntervalValueStateV3::sql_null,r.value.months,r.value.civil_days,r.value.fixed_nanoseconds};break;}
    case dt::IntervalIntrinsicOperationV3::add:
    case dt::IntervalIntrinsicOperationV3::subtract:{
      dt::IntervalOwnedValueV3 zero{profile,dt::IntervalValueStateV3::value,0,0,0};
      auto r=op==dt::IntervalIntrinsicOperationV3::add?dt::AddIntervalCheckedV3(value,zero,true,control):dt::SubtractIntervalCheckedV3(value,zero,true,control);out={r.ok(),r.diagnostic.diagnostic_code,Default(r),r.value.state==dt::IntervalValueStateV3::sql_null,r.value.months,r.value.civil_days,r.value.fixed_nanoseconds};break;}
    default:{
      auto r=dt::RefuseIntervalIntrinsicOperationV3(value.view(),op,control);out={r.ok(),r.diagnostic.diagnostic_code,Default(r),false,0,0,0};break;}
  }
  return out;
}

void ExpectFailure(const Observation&o,std::string_view diagnostic,std::string_view what){Check(!o.ok&&o.diagnostic==diagnostic&&o.default_output,what);++vectors;}

void Classification(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile){
  for(unsigned n=0;n<13;++n){auto op=static_cast<dt::IntervalIntrinsicOperationV3>(n);auto expected=n<7?dt::IntervalIntrinsicDispositionV3::admitted:dt::IntervalIntrinsicDispositionV3::registered_refused;Check(dt::ClassifyIntervalIntrinsicOperationV3(op)==expected,"closed named intrinsic classifier");}
  Check(dt::ClassifyIntervalIntrinsicOperationV3(static_cast<dt::IntervalIntrinsicOperationV3>(255))==dt::IntervalIntrinsicDispositionV3::unknown,"unknown intrinsic classifier");
  auto value=dt::ConstructIntervalV3(profile,1,-2,3);Check(value.ok(),"refusal misuse source");for(unsigned n=0;n<7;++n){auto r=dt::RefuseIntervalIntrinsicOperationV3(value.value.view(),static_cast<dt::IntervalIntrinsicOperationV3>(n));Check(Diagnostic(r,"SBLR.OPERAND_INVALID")&&Default(r),"admitted refusal API misuse");}
}

void BaseVectors(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& p){
  auto full=dt::ConstructIntervalV3(p,INT32_MIN,INT32_MIN,INT64_MIN);Check(full.ok()&&full.value.months==INT32_MIN&&full.value.civil_days==INT32_MIN&&full.value.fixed_nanoseconds==INT64_MIN,"construct_full_cartesian_min");++vectors;
  auto mixed=dt::ConstructIntervalV3(p,-7,8,-9);Check(mixed.ok(),"mixed source");auto d=dt::DecomposeIntervalV3(mixed.value.view());Check(d.ok()&&!d.is_null&&d.component.months==-7&&d.component.civil_days==8&&d.component.fixed_nanoseconds==-9,"decompose_mixed");++vectors;auto neg=dt::NegateIntervalCheckedV3(mixed.value);Check(neg.ok()&&neg.value.months==7&&neg.value.civil_days==-8&&neg.value.fixed_nanoseconds==9,"negate_mixed");++vectors;
  for(const auto&v:std::array<std::array<std::int64_t,3>,3>{{{{INT32_MIN,0,0}},{{0,INT32_MIN,0}},{{0,0,INT64_MIN}}}}){auto source=dt::ConstructIntervalV3(p,v[0],v[1],v[2]);Check(source.ok(),"negate minimum source");auto r=dt::NegateIntervalCheckedV3(source.value);Check(Diagnostic(r,"CTI.TEMPORAL.RANGE_EXCEEDED")&&Default(r),"negate component minimum");++vectors;}
  auto left=dt::ConstructIntervalV3(p,0,0,86399999999999ll),right=dt::ConstructIntervalV3(p,0,0,1);Check(left.ok()&&right.ok(),"nanosecond carry sources");auto add=dt::AddIntervalCheckedV3(left.value,right.value);Check(add.ok()&&add.value.months==0&&add.value.civil_days==0&&add.value.fixed_nanoseconds==86400000000000ll,"add_no_ns_carry");++vectors;
  left=dt::ConstructIntervalV3(p,0,30,0);right=dt::ConstructIntervalV3(p,0,1,0);Check(left.ok()&&right.ok(),"day carry sources");add=dt::AddIntervalCheckedV3(left.value,right.value);Check(add.ok()&&add.value.months==0&&add.value.civil_days==31&&add.value.fixed_nanoseconds==0,"add_no_day_carry");++vectors;
  left=dt::ConstructIntervalV3(p,INT32_MAX,4,5);right=dt::ConstructIntervalV3(p,1,6,7);Check(left.ok()&&right.ok(),"atomic add sources");add=dt::AddIntervalCheckedV3(left.value,right.value);Check(Diagnostic(add,"CTI.TEMPORAL.RANGE_EXCEEDED")&&Default(add),"add_month_overflow_atomic");++vectors;
  left=dt::ConstructIntervalV3(p,1,2,INT64_MIN);right=dt::ConstructIntervalV3(p,0,0,1);Check(left.ok()&&right.ok(),"atomic subtract sources");auto sub=dt::SubtractIntervalCheckedV3(left.value,right.value);Check(Diagnostic(sub,"CTI.TEMPORAL.RANGE_EXCEEDED")&&Default(sub),"subtract_ns_overflow_atomic");++vectors;
  auto refused=Run(dt::IntervalIntrinsicOperationV3::apply_to_temporal,p,dt::IntervalValueStateV3::value,Fault::none);ExpectFailure(refused,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED","temporal_application_refused");
}

void OperationMatrix(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& p){
  for(unsigned n=0;n<13;++n){
    const auto op=static_cast<dt::IntervalIntrinsicOperationV3>(n);
    auto present=Run(op,p,dt::IntervalValueStateV3::value,Fault::none);
    auto nullv=Run(op,p,dt::IntervalValueStateV3::sql_null,Fault::none);
    if(n<7){const bool exact=n==4?present.months==-1&&present.civil_days==2&&present.nanoseconds==-3:present.months==1&&present.civil_days==-2&&present.nanoseconds==3;Check(present.ok&&!present.is_null&&exact,"intrinsic PRESENT exact");++vectors;Check(nullv.ok&&nullv.is_null&&nullv.months==0&&nullv.civil_days==0&&nullv.nanoseconds==0,"intrinsic SQL NULL exact");++vectors;}
    else{const auto code=n==7?std::string_view{"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED"}:std::string_view{"CTI.TEMPORAL.ORDERING_REFUSED"};ExpectFailure(present,code,"refused PRESENT classification");ExpectFailure(nullv,code,"classification before NULL propagation");}
    ExpectFailure(Run(op,p,dt::IntervalValueStateV3::value,Fault::wrong_receipt),"CTI.INTERVAL.DESCRIPTOR_INVALID","wrong receipt matrix");
    ExpectFailure(Run(op,p,dt::IntervalValueStateV3::value,Fault::wrong_profile),"CTI.INTERVAL.DESCRIPTOR_INVALID","wrong profile matrix");
    ExpectFailure(Run(op,p,dt::IntervalValueStateV3::sql_null,Fault::dirty_null),"DATATYPE.NULL_STATE.INVALID","dirty NULL matrix");
    ExpectFailure(Run(op,p,dt::IntervalValueStateV3::value,Fault::resource),"RESOURCE.BUDGET_EXCEEDED","resource one-short matrix");
    ExpectFailure(Run(op,p,dt::IntervalValueStateV3::value,Fault::cancel),"PROCESS.CANCELLED","final cancellation matrix");
  }
}

dt::IntervalValueResultV3 ConstructCarrierRange(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>&p,unsigned component,bool above){
  dt::IntervalNullableI32FactV3 m,d;dt::IntervalNullableI64FactV3 n;
  if(component==0)m.carrier=above?dt::IntervalI32CarrierKindV3::above_i32:dt::IntervalI32CarrierKindV3::below_i32;
  if(component==1)d.carrier=above?dt::IntervalI32CarrierKindV3::above_i32:dt::IntervalI32CarrierKindV3::below_i32;
  if(component==2)n.carrier=above?dt::IntervalI64CarrierKindV3::above_i64:dt::IntervalI64CarrierKindV3::below_i64;
  return dt::ConstructIntervalV3(p,m,d,n);
}
dt::IntervalOwnedValueV3 Value(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>&p,std::int32_t m,std::int32_t d,std::int64_t n){return {p,dt::IntervalValueStateV3::value,m,d,n};}
void RangeFailure(const dt::IntervalValueResultV3&r,std::string_view what){Check(Diagnostic(r,"CTI.TEMPORAL.RANGE_EXCEEDED")&&Default(r),what);++vectors;}

void OverflowVectors(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>&p){
  for(unsigned component=0;component<3;++component)RangeFailure(ConstructCarrierRange(p,component,true),"construct one past signed bound");
  for(const auto&v:std::array<dt::IntervalOwnedValueV3,3>{{Value(p,INT32_MIN,0,0),Value(p,0,INT32_MIN,0),Value(p,0,0,INT64_MIN)}})RangeFailure(dt::NegateIntervalCheckedV3(v),"negate one past signed bound");
  for(unsigned component=0;component<3;++component){auto max=Value(p,component==0?INT32_MAX:0,component==1?INT32_MAX:0,component==2?INT64_MAX:0);auto one=Value(p,component==0?1:0,component==1?1:0,component==2?1:0);RangeFailure(dt::AddIntervalCheckedV3(max,one),"add one past signed bound");}
  for(unsigned component=0;component<3;++component){auto min=Value(p,component==0?INT32_MIN:0,component==1?INT32_MIN:0,component==2?INT64_MIN:0);auto one=Value(p,component==0?1:0,component==1?1:0,component==2?1:0);RangeFailure(dt::SubtractIntervalCheckedV3(min,one),"subtract one past signed bound");}

  RangeFailure(dt::ConstructIntervalV3(p,std::int64_t{INT32_MAX}+1,0,0),"construct months above exact");RangeFailure(dt::ConstructIntervalV3(p,std::int64_t{INT32_MIN}-1,0,0),"construct months below exact");
  RangeFailure(dt::ConstructIntervalV3(p,0,std::int64_t{INT32_MAX}+1,0),"construct days above exact");RangeFailure(dt::ConstructIntervalV3(p,0,std::int64_t{INT32_MIN}-1,0),"construct days below exact");
  RangeFailure(ConstructCarrierRange(p,2,true),"construct nanoseconds above exact");RangeFailure(ConstructCarrierRange(p,2,false),"construct nanoseconds below exact");
  for(unsigned component=0;component<3;++component){auto max=Value(p,component==0?INT32_MAX:0,component==1?INT32_MAX:0,component==2?INT64_MAX:0);auto min=Value(p,component==0?INT32_MIN:0,component==1?INT32_MIN:0,component==2?INT64_MIN:0);auto pos=Value(p,component==0?1:0,component==1?1:0,component==2?1:0);auto neg=Value(p,component==0?-1:0,component==1?-1:0,component==2?-1:0);RangeFailure(dt::AddIntervalCheckedV3(max,pos),"add positive overflow exact");RangeFailure(dt::AddIntervalCheckedV3(min,neg),"add negative overflow exact");}
  for(unsigned component=0;component<3;++component){auto max=Value(p,component==0?INT32_MAX:0,component==1?INT32_MAX:0,component==2?INT64_MAX:0);auto min=Value(p,component==0?INT32_MIN:0,component==1?INT32_MIN:0,component==2?INT64_MIN:0);auto pos=Value(p,component==0?1:0,component==1?1:0,component==2?1:0);auto neg=Value(p,component==0?-1:0,component==1?-1:0,component==2?-1:0);RangeFailure(dt::SubtractIntervalCheckedV3(max,neg),"subtract positive overflow exact");RangeFailure(dt::SubtractIntervalCheckedV3(min,pos),"subtract negative overflow exact");}
}

void PrecedenceAndUnknown(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>&p){
  dt::IntervalExecutionControlV3 both;both.maximum_allocation_bytes=15;both.cancelled=Cancel;
  auto invalid=std::make_shared<dt::IntervalValidatedProfileHandleV3>(*p);invalid->profile_fingerprint[0]^=1;auto bad=Value(invalid,INT32_MAX,0,0),one=Value(invalid,1,0,0);auto r=dt::AddIntervalCheckedV3(bad,one,true,both);Check(Diagnostic(r,"CTI.INTERVAL.DESCRIPTOR_INVALID")&&Default(r),"profile before range resource cancel");++vectors;
  auto max=Value(p,INT32_MAX,0,0);one=Value(p,1,0,0);r=dt::AddIntervalCheckedV3(max,one,true,both);Check(Diagnostic(r,"CTI.TEMPORAL.RANGE_EXCEEDED")&&Default(r),"range before resource cancel");++vectors;
  r=dt::ConstructIntervalV3(p,0,0,0,true,both);Check(Diagnostic(r,"RESOURCE.BUDGET_EXCEEDED")&&Default(r),"resource before cancel");++vectors;
  auto unknown=dt::RefuseIntervalIntrinsicOperationV3(Value(p,1,-2,3).view(),static_cast<dt::IntervalIntrinsicOperationV3>(255));Check(Diagnostic(unknown,"CTI.INTERVAL.CALENDAR_OPERATION_REFUSED")&&Default(unknown),"unknown operation fail closed");++vectors;
}

void Extractors(const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>&p){auto v=Value(p,-12,34,-56);auto m=dt::ExtractIntervalMonthsV3(v.view()),d=dt::ExtractIntervalCivilDaysV3(v.view()),n=dt::ExtractIntervalFixedNanosecondsV3(v.view());Check(m.ok()&&m.kind==dt::IntervalScalarKindV3::months_i32&&m.signed_i32_value==-12&&m.signed_i64_value==0,"months extraction");Check(d.ok()&&d.kind==dt::IntervalScalarKindV3::civil_days_i32&&d.signed_i32_value==34&&d.signed_i64_value==0,"days extraction");Check(n.ok()&&n.kind==dt::IntervalScalarKindV3::fixed_nanoseconds_i64&&n.signed_i32_value==0&&n.signed_i64_value==-56,"nanoseconds extraction");}

} // namespace

int main(){auto p=Profile();Classification(p);BaseVectors(p);OperationMatrix(p);OverflowVectors(p);PrecedenceAndUnknown(p);Extractors(p);Check(vectors==136,"all 136 Core intrinsic vectors executed");std::cout<<"PASS checks="<<checks<<" intrinsic_vectors="<<vectors<<" resource_cancel_all_13=true atomic_defaults=true\n";}
