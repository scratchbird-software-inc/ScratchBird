// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query/expression_api.hpp"
#include "descriptor_value_runtime.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <bit>
#include <chrono>
#include <iostream>
#include <limits>
#include <cstdlib>
#include <new>
#include <stdexcept>

long allocation_budget=-1;
void* operator new(std::size_t n) {
  if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}
  if(allocation_budget>0)--allocation_budget;
  if(auto* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
unsigned checks=0;
void Check(bool value, const char* message) {++checks;if(!value)throw std::runtime_error(message);}
std::vector<std::uint8_t> Bytes(std::int32_t value) {
  const auto bits=std::bit_cast<std::uint32_t>(value);
  return {static_cast<std::uint8_t>(bits),static_cast<std::uint8_t>(bits>>8),
          static_cast<std::uint8_t>(bits>>16),static_cast<std::uint8_t>(bits>>24)};
}
api::EngineExtractValueRequest Request(unsigned cohort=10) {
  api::EngineExtractValueRequest r;
  r.context.security_context_present=true;
  r.context.datatype_catalog_snapshot_uuid=cohort==11
      ? scratchbird::tests::FixtureUuidLiteral("01a1095f-f205-72d3-abea-15c6d41a4ad4")
      : scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d710");
  if(cohort>=7 && cohort<=9) r.context.datatype_catalog_snapshot_uuid.bytes[15]=cohort;
  r.context.datatype_catalog_generation=cohort;r.context.datatype_registry_generation=cohort;
  r.input_value.descriptor=exec::MakeExecutorDescriptor("date","nullability=nullable");
  r.input_value.descriptor.descriptor_kind="scalar";
  api::CatalogColumnMetadata fields;
  fields.text={{"nullability","nullable"},{"codec_generation","1"}};
  fields.identities["codec_uuid"]=scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81d");
  Check(api::EncodeCatalogColumnMetadata(fields,&r.input_value.descriptor.encoded_descriptor),"encode DATE metadata");
  r.input_value.binary_value=Bytes(0);r.field="year";
  r.descriptors={exec::MakeExecutorDescriptor("int32","nullability=nullable")};
  r.descriptors.front().descriptor_kind="scalar";
  return r;
}
void Expect(const api::EngineExtractValueRequest& r, std::int32_t expected) {
  const auto result=api::EngineExtractValue(r);
  if(!result.ok)for(const auto&d:result.diagnostics)std::cerr<<d.code<<":"<<d.detail<<"\n";
  Check(result.ok && result.value.state==api::EngineValueState::value && !result.value.is_null &&
        result.value.descriptor==r.descriptors.front() && result.value.encoded_value.empty() &&
        result.value.binary_value==Bytes(expected),"native DATE extraction value/descriptor mismatch");
}
void Calendar() {
  for(unsigned cohort : {7,8,9,10,11}) {
    auto r=Request(cohort);
    for(int year : {-32767,-10000,-400,-100,-4,-1,0,1,4,100,400,1582,1900,1969,1970,2000,2026,9999,32767})
      for(unsigned month=1;month<=12;++month)for(unsigned day : {1,28}) {
        const std::chrono::year_month_day civil{std::chrono::year(year),std::chrono::month(month),std::chrono::day(day)};
        r.input_value.binary_value=Bytes(static_cast<std::int32_t>(std::chrono::sys_days(civil).time_since_epoch().count()));
        r.field="year";Expect(r,year);r.field="month";Expect(r,month);r.field="day";Expect(r,day);
      }
    r.input_value.binary_value=Bytes(std::numeric_limits<std::int32_t>::min());
    r.field="YYYY";Expect(r,-5877641);r.field="MM";Expect(r,6);r.field="DD";Expect(r,23);
    r.input_value.binary_value=Bytes(std::numeric_limits<std::int32_t>::max());
    r.field="year";Expect(r,5881580);r.field="month";Expect(r,7);r.field="day";Expect(r,11);
    r.input_value.binary_value.clear();r.input_value.setState(api::EngineValueState::sql_null);
    const auto null=api::EngineExtractValue(r);
    Check(null.ok && null.value.isSqlNull() && null.value.descriptor==r.descriptors.front() &&
          null.value.binary_value.empty() && null.value.encoded_value.empty(),"DATE NULL not propagated");
  }
}
void Refusals() {
  for(unsigned mutation=0;mutation<21;++mutation)for(bool is_null : {false,true}) {
    auto r=Request();const char* code="CTI.TEMPORAL.DESCRIPTOR_INVALID";
    if(is_null){r.input_value.binary_value.clear();r.input_value.setState(api::EngineValueState::sql_null);}
    api::CatalogColumnMetadata fields;
    Check(api::DecodeCatalogColumnMetadata(r.input_value.descriptor.encoded_descriptor,&fields),"decode DATE metadata");
    switch(mutation) {
      case 0: r.context.datatype_catalog_snapshot_uuid={};break;
      case 1: ++r.context.datatype_catalog_generation;break;
      case 2: ++r.context.datatype_registry_generation;break;
      case 3: ++r.input_value.descriptor.datatype_descriptor_generation;break;
      case 4: r.input_value.descriptor.type_uuid.bytes[0]^=1;break;
      case 5: fields.identities["codec_uuid"].bytes[0]^=1;break;
      case 6: fields.text["codec_generation"]="2";break;
      case 7: fields.text["calendar"]="foreign";break;
      case 8: fields.identities["domain_uuid"]=r.input_value.descriptor.type_uuid;break;
      case 9: fields.identities.erase("codec_uuid");break;
      case 10: r.input_value.encoded_value="1970-01-01";
               code=is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID";break;
      case 11: r.input_value.binary_value.push_back(0);
               code=is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID";break;
      case 12: r.field="hour";code="CTI.TEMPORAL.OPERATION_REFUSED";break;
      case 13: r.descriptors.clear();code="DATATYPE.DESCRIPTOR.INVALID";break;
      case 14: ++r.descriptors[0].datatype_descriptor_generation;code="DATATYPE.DESCRIPTOR.INVALID";break;
      case 15: r.descriptors[0].encoded_descriptor="nullability=nullable;width=128";
               code="DATATYPE.DESCRIPTOR.INVALID";break;
      case 16: r.input_value.is_null=!r.input_value.is_null;code="DATATYPE.NULL_STATE.INVALID";break;
      case 17: r.context.datatype_catalog_snapshot_uuid=
               scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d705");
               r.context.datatype_catalog_generation=5;r.context.datatype_registry_generation=5;break;
      case 18: r.descriptors[0].charset_uuid=r.input_value.descriptor.type_uuid;
               code="DATATYPE.DESCRIPTOR.INVALID";break;
      case 19: fields.text["codec_version"]="2";break;
      case 20: r.descriptors[0].collation_uuid=r.input_value.descriptor.type_uuid;
               code="DATATYPE.DESCRIPTOR.INVALID";break;
    }
    Check(api::EncodeCatalogColumnMetadata(fields,&r.input_value.descriptor.encoded_descriptor),"encode bad DATE metadata");
    const auto result=api::EngineExtractValue(r);
    if(result.ok || result.diagnostics.empty() || result.diagnostics.front().code!=code)
      std::cerr<<"mutation="<<mutation<<" null="<<is_null<<" code="
          <<(result.diagnostics.empty()?"none":result.diagnostics.front().code)<<"\n";
    Check(!result.ok && result.value.binary_value.empty() && result.value.encoded_value.empty() &&
          !result.diagnostics.empty() && result.diagnostics.front().code==code,"DATE refusal lost exact diagnostic or published result");
  }
  for(unsigned slot=0;slot<2;++slot) {
    auto r=Request();
    if(slot==0) {
      api::CatalogColumnMetadata fields;
      Check(api::DecodeCatalogColumnMetadata(r.input_value.descriptor.encoded_descriptor,&fields),"decode nullability");
      fields.text["nullability"]="non_null";
      Check(api::EncodeCatalogColumnMetadata(fields,&r.input_value.descriptor.encoded_descriptor),"encode nullability");
    } else r.descriptors[0].encoded_descriptor="nullability=non_null";
    r.input_value.binary_value.clear();r.input_value.setState(api::EngineValueState::sql_null);
    const auto result=api::EngineExtractValue(r);
    Check(!result.ok && !result.diagnostics.empty() && result.diagnostics.front().code=="DATATYPE.NULL_NOT_ADMITTED",
          "DATE nullability ignored");
  }
  for(bool throwing : {false,true}) {
    auto r=Request();unsigned calls=0;
    r.context.query_cancellation_requested=[&] {
      ++calls;
      if(throwing)throw std::runtime_error("cancel callback");
      return true;
    };
    const auto result=api::EngineExtractValue(r);
    Check(!result.ok && calls==1 && !result.diagnostics.empty() &&
          result.diagnostics.front().code=="PROCESS.CANCELLED" &&
          result.value.binary_value.empty() && result.value.encoded_value.empty(),
          "DATE cancellation or callback exception published result");
    r.input_value.binary_value.push_back(0);calls=0;
    const auto invalid=api::EngineExtractValue(r);
    Check(!invalid.ok && calls==0 && !invalid.diagnostics.empty() &&
          invalid.diagnostics.front().code=="CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
          "DATE cancellation hid higher-priority malformed input");
  }
  for(unsigned cohort : {4,5,6}) {
    auto r=Request();r.context.datatype_catalog_snapshot_uuid.bytes[15]=cohort;
    r.context.datatype_catalog_generation=cohort;r.context.datatype_registry_generation=cohort;
    const auto result=api::EngineExtractValue(r);
    Check(!result.ok && !result.diagnostics.empty() &&
          result.diagnostics.front().code=="CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "storage-only DATE receipt acquired semantic authority");
  }
}
void AllocationFailures() {
  auto r=Request();const auto input=r.input_value;bool success=false;unsigned failures=0;
  for(long budget=0;budget<500 && !success;++budget) {
    allocation_budget=budget;
    const auto result=api::EngineExtractValue(r);
    allocation_budget=-1;
    if(result.ok){success=true;Check(result.value.binary_value==Bytes(1970),"DATE allocation sweep successful value");}
    else {
      ++failures;
      const bool resource=!result.diagnostics.empty() &&
          (result.diagnostics.front().code=="DATATYPE.RESOURCE_EXHAUSTED" ||
           result.diagnostics.front().code=="RESOURCE.BUDGET_EXCEEDED");
      if(!resource)
        std::cerr<<"allocation budget="<<budget<<" code="
            <<(result.diagnostics.empty()?"none":result.diagnostics.front().code)<<"\n";
      Check(resource &&
            result.value.binary_value.empty() && result.value.encoded_value.empty(),
            "DATE allocation failure published bytes or lost resource classification");
    }
    Check(r.input_value.binary_value==input.binary_value && r.input_value.descriptor==input.descriptor,
          "DATE allocation failure changed source");
  }
  Check(success && failures>1,"DATE allocation sweep incomplete");
}
int main() try { Calendar();Refusals();AllocationFailures();std::cout<<checks<<" native DATE extraction checks passed\n"; }
catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<" after "<<checks<<" checks\n";return 1;}
