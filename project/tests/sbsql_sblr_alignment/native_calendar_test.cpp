// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr/sblr_temporal_cast.hpp"
#include "sblr/sblr_binary_value_carrier.hpp"
#include "executor/descriptor_value_runtime.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "registry/function_seed_registry.hpp"
#include "dispatch/function_dispatch.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#include <new>

namespace allocation_fault {
thread_local int remaining=-1;
thread_local bool fired=false;
thread_local bool counting=false;
thread_local unsigned count=0;
}
void* operator new(std::size_t size) {
  if(allocation_fault::counting)++allocation_fault::count;
  if(allocation_fault::remaining>=0 && allocation_fault::remaining--==0) {
    allocation_fault::fired=true;
    throw std::bad_alloc();
  }
  if(void* memory=std::malloc(size?size:1))return memory;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace s = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace f = scratchbird::engine::functions;
namespace ex = scratchbird::engine::executor;
namespace {
unsigned checks = 0;
bool direct=false;
constexpr std::array names{"sb.temporal.dow", "sb.temporal.doy", "sb.temporal.quarter",
                           "sb.temporal.isodow", "sb.temporal.week"};
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}
std::shared_ptr<const api::EngineDescriptor> Binding(const char* name, unsigned generation,
                                                   bool nullable = true) {
  api::EngineDatatypeCohort cohort{
    scratchbird::core::platform::Uuid{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,
      static_cast<std::uint8_t>(generation==10?0x10:generation==11?0x11:generation)}}, generation,generation};
  if(generation==11)cohort={dt::kDatatypeCohortV11,11,11};
  auto binding = ex::MakeExecutorDescriptor(name,
      nullable ? "nullability=nullable" : "nullability=non_null", cohort);
  binding.descriptor_uuid = scratchbird::tests::FixtureUuid(2190, name[0]);
  return std::make_shared<const api::EngineDescriptor>(std::move(binding));
}
s::SblrValue Native(const char* name, std::int32_t day, unsigned generation) {
  s::SblrValue value;
  value.descriptor_id=name;
  value.projection_descriptor=Binding(name,generation);
  value.is_null=false;
  value.payload_kind=s::SblrValuePayloadKind::binary;
  value.binary_value.resize(value.descriptor_id=="date"?4:16);
  const auto bits=std::bit_cast<std::uint32_t>(day);
  for(unsigned i=0;i<4;++i)value.binary_value[i]=bits>>(8*i);
  if(value.descriptor_id=="timestamp") {
    // Portable civil seconds, nanosecond-of-second, then four reserved zeros.
    const auto seconds=std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(day)*86400+86399);
    for(unsigned i=0;i<8;++i)value.binary_value[i]=seconds>>(8*i);
    constexpr std::uint32_t nanos=999999999;
    for(unsigned i=0;i<4;++i)value.binary_value[8+i]=nanos>>(8*i);
  }
  return value;
}
s::SblrResult Run(unsigned operation, const s::SblrValue& input,
                  const std::shared_ptr<const api::EngineDescriptor>& target) {
  static const s::SblrExecutionContext context;
  if(direct)return s::EvaluateSblrTemporalCalendar("calendar",static_cast<s::SblrCalendarField>(operation),
                                                 input,context,target);
  static const auto package=f::BuildStandardFunctionSeedPackage();
  const auto* entry=package.registry.Lookup(names[operation]);
  Check(entry!=nullptr,"calendar function not registered");
  f::FunctionCallRequest request;
  request.context.function_uuid=entry->function_uuid;
  request.context.security_allowed=request.context.policy_allowed=true;
  Check(package.registry.BindCallContext(request.context)!=nullptr,"function binding failed");
  request.result_descriptor=target;
  request.arguments={{"source",input}};
  return f::DispatchFunctionCall(package.registry,std::move(request)).result;
}
const s::SblrValue& Scalar(const s::SblrResult& result) {
  if(!result.ok())for(const auto& d:result.diagnostics)std::cerr<<d.diagnostic_id<<':'<<d.detail<<'\n';
  Check(result.ok() && result.scalar_values.size()==1 && result.rows.empty() &&
        !result.mutation_attempted && !result.mutation_committed,"native calendar failed");
  return result.scalar_values.front();
}
void Refused(const s::SblrResult& result,const char* code) {
  Check(!result.ok() && result.scalar_values.empty() && result.rows.empty() &&
        !result.mutation_attempted && !result.mutation_committed && !result.diagnostics.empty(),
        "invalid calendar published output");
  Check(result.diagnostics.front().diagnostic_id==code,"wrong calendar diagnostic");
}
unsigned AllocationSweep(unsigned op,const s::SblrValue& input,
                         const std::shared_ptr<const api::EngineDescriptor>& target) {
  Run(op,input,target); // Warm unrelated process statics before measuring.
  allocation_fault::count=0;allocation_fault::counting=true;
  const auto baseline=Run(op,input,target);
  allocation_fault::counting=false;
  const auto extent=allocation_fault::count;
  const auto& expected=Scalar(baseline);
  Check(extent>0 && extent<4096,"invalid allocation sweep extent");
  for(unsigned position=0;position<=extent;++position) {
    allocation_fault::remaining=position;allocation_fault::fired=false;
    const auto result=Run(op,input,target);
    allocation_fault::remaining=-1;
    Check(allocation_fault::fired==(position<extent),"allocation sweep missed an allocation");
    if(position==extent) {
      const auto& out=Scalar(result);
      Check(out.is_null==expected.is_null && out.int64_value==expected.int64_value &&
            out.has_int64_value==expected.has_int64_value && out.projection_descriptor==target,
            "allocation boundary success changed result");
    } else {
      Refused(result,"RESOURCE.BUDGET_EXCEEDED");
      Check(result.status==s::SblrStatusCode::resource_exhausted,"lost allocation failure classification");
    }
  }
  return extent;
}
void Negative(const char* name,unsigned generation) {
  const auto target=Binding("int64",generation);
  const auto input=Native(name,0,generation);
  auto null=input;null.is_null=true;null.binary_value.clear();null.payload_kind=s::SblrValuePayloadKind::none;
  for(unsigned op=0;op<names.size();++op) {
    Check(s::SblrNullPayloadEmpty(Scalar(Run(op,null,target))),"calendar did not produce clean NULL");
    Refused(Run(op,null,Binding("int64",generation,false)),"DATATYPE.NULL_NOT_ADMITTED");
    auto required=null;required.projection_descriptor=Binding(name,generation,false);
    Refused(Run(op,required,target),"DATATYPE.NULL_NOT_ADMITTED");
    auto present=input;present.projection_descriptor=required.projection_descriptor;
    Check(!Scalar(Run(op,present,Binding("int64",generation,false))).is_null,"nonnullable PRESENT refused");
    auto dirty=null;dirty.binary_value={0};
    Refused(Run(op,dirty,target),"DATATYPE.NULL_STATE.INVALID");
    Refused(Run(op,input,{}),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
    Refused(Run(op,input,Binding("int32",generation)),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
    for(unsigned mutation=0;mutation<9;++mutation)for(bool on_target:{false,true})for(bool sql_null:{false,true}) {
      auto source=sql_null?null:input;
      auto invalid=std::make_shared<api::EngineDescriptor>(*(on_target?target:input.projection_descriptor));
      switch(mutation) {
        case 0:invalid->descriptor_uuid={};break;
        case 1:invalid->type_uuid.bytes[15]^=0x7f;break;
        case 2:invalid->datatype_descriptor_uuid.bytes[15]^=0x7f;break;
        case 3:++invalid->datatype_descriptor_generation;break;
        case 4:invalid->datatype_cohort.catalog_snapshot_uuid.bytes[15]^=0x7f;break;
        case 5:++invalid->datatype_cohort.catalog_generation;break;
        case 6:++invalid->datatype_cohort.registry_generation;break;
        case 7:invalid->encoded_descriptor+=";length=8";break;
        case 8:invalid->charset_uuid=scratchbird::tests::FixtureUuid(2191,1);break;
      }
      if(on_target)Refused(Run(op,source,invalid),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
      else {source.projection_descriptor=invalid;Refused(Run(op,source,target),"CTI.TEMPORAL.DESCRIPTOR_INVALID");}
    }
    for(unsigned mutation=0;mutation<7;++mutation) {
      auto bad=input;
      switch(mutation) {
        case 0:bad.binary_value.pop_back();break;
        case 1:bad.text_value="1970-01-01";break;
        case 2:bad.encoded_value="1970-01-01";break;
        case 3:bad.has_int64_value=true;break;
        case 4:bad.payload_kind=s::SblrValuePayloadKind::text;break;
        case 5:bad.charset_name="UTF8";break;
        case 6:bad.uuid_value=scratchbird::tests::FixtureUuid(2191,2);break;
      }
      Refused(Run(op,bad,target),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
    }
    auto missing=input;missing.projection_descriptor.reset();
    Refused(Run(op,missing,target),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
    if(std::string_view(name)=="timestamp") {
      auto bad=input;bad.binary_value[12]=1;
      Refused(Run(op,bad,target),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
      bad=input;
      for(unsigned i=0;i<4;++i)bad.binary_value[8+i]=1000000000u>>(8*i);
      Refused(Run(op,bad,target),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
      for(const std::int64_t second:{-185542587187201LL,185542587187200LL}) {
        bad=input;
        const auto bits=std::bit_cast<std::uint64_t>(second);
        for(unsigned i=0;i<8;++i)bad.binary_value[i]=bits>>(8*i);
        Refused(Run(op,bad,target),"CTI.TEMPORAL.CANONICAL_ENCODING_INVALID");
      }
      auto midnight=input;
      midnight.binary_value.assign(16,0);
      Check(Scalar(Run(op,midnight,target)).int64_value==Scalar(Run(op,input,target)).int64_value,
            "subday time changed a calendar fact");
    }
    // Exact source/result receipts must belong to the same admitted cohort.
    const auto current=Binding("int64",11);
    if(generation==11)Check(Scalar(Run(op,input,current)).projection_descriptor==current,"result binding lost");
    else Refused(Run(op,input,current),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
  }
}
std::array<std::int64_t,5> Oracle(std::int32_t day) {
  using namespace std::chrono;
  // All five facts repeat after the Gregorian 400-year cycle (146097 days).
  // Reduce to a chrono-supported year; no production calendar helper is used.
  const auto reduced=(static_cast<std::int64_t>(day)%146097+146097)%146097;
  const sys_days date{days{reduced}};
  const year_month_day civil{date};
  const auto iso=weekday{date}.iso_encoding();
  const sys_days thursday=date+days{4-static_cast<int>(iso)};
  const auto iso_year=year_month_day{thursday}.year();
  const sys_days jan4=iso_year/January/4;
  const auto monday=jan4-days{static_cast<int>(weekday{jan4}.iso_encoding())-1};
  return {weekday{date}.c_encoding(),(date-sys_days{civil.year()/January/1}).count()+1,
          (static_cast<unsigned>(civil.month())-1)/3+1,iso,(date-monday).count()/7+1};
}
}
int main() try {
  std::vector<std::int32_t> days{0,-1,1,10957,11016,16070,16436,17166,18262,
      std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()};
  for(int day=-800;day<=800;day+=7)days.push_back(day);
  for(bool route:{false,true}) {
  direct=route;
  for(const auto* name: {"date","timestamp"}) for(unsigned generation:{7u,8u,9u,10u,11u}) {
    if(std::string_view(name)=="timestamp" && generation<10)continue;
    const auto target=Binding("int64",generation);
    Negative(name,generation);
    for(const auto day:days) {
      const auto expected=Oracle(day);
      const auto input=Native(name,day,generation);
      for(unsigned op=0;op<names.size();++op) {
        const auto result=Run(op,input,target);
        const auto& out=Scalar(result);
        Check(!out.is_null && out.has_int64_value && out.int64_value==expected[op] &&
              out.payload_kind==s::SblrValuePayloadKind::signed_integer &&
              out.projection_descriptor==target && out.descriptor_id=="int64" &&
              out.text_value.empty() && out.encoded_value.empty() && out.binary_value.empty() &&
              !out.has_real64_value && !out.has_uint64_value,"calendar fact or carrier mismatch");
      }
    }
  }
  }
  direct=true;
  unsigned failures=0;
  for(const auto* name:{"date","timestamp"}) {
    const auto input=Native(name,0,11);
    auto null=input;null.is_null=true;null.binary_value.clear();null.payload_kind=s::SblrValuePayloadKind::none;
    for(unsigned op=0;op<names.size();++op)for(const auto& value:{input,null})
      failures+=AllocationSweep(op,value,Binding("int64",11));
    Refused(Run(99,input,Binding("int64",11)),"CTI.TEMPORAL.OPERATION_REFUSED");
    auto missing=input;missing.projection_descriptor.reset();
    Refused(Run(99,missing,Binding("int64",11)),"CTI.TEMPORAL.DESCRIPTOR_INVALID");
    auto dirty=null;dirty.binary_value={0};
    Refused(Run(99,dirty,Binding("int64",11)),"DATATYPE.NULL_STATE.INVALID");
  }
  std::cout<<"native calendar checks="<<checks<<" allocation_failures="<<failures<<'\n';
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
