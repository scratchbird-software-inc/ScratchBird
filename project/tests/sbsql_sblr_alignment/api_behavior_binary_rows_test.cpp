// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "behavior_support/api_behavior_store.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>

namespace { long fail_after=-1; unsigned checks=0,failures=0,faults=0; }
void* operator new(std::size_t n) {
  if(fail_after==0)throw std::bad_alloc();
  if(fail_after>0)--fail_after;
  if(auto* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace api=scratchbird::engine::internal_api;
namespace {
void Check(bool good,const char* detail) {
  ++checks;
  if(!good){++failures;std::cerr<<"FAIL "<<detail<<'\n';}
}
api::EngineUuid Id(unsigned value) {
  api::EngineUuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[15]=value;
  return id;
}
bool Binary(const api::EngineTypedValue& value,const api::EngineUuid& id) {
  return value.encoded_value.empty() && value.binary_value.size()==16 &&
      std::equal(id.bytes.begin(),id.bytes.end(),value.binary_value.begin());
}
bool Unbound(const api::EngineDescriptor& d) {
  return d.descriptor_uuid.is_nil() && d.type_uuid.is_nil() && d.datatype_descriptor_uuid.is_nil() &&
      d.datatype_descriptor_generation==0 && d.charset_uuid.is_nil() && d.collation_uuid.is_nil();
}
bool SameEvidence(std::span<const api::EngineEvidenceReference> a,
                  std::span<const api::EngineEvidenceReference> b) {
  return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
      [](const auto& x,const auto& y){return x.evidence_kind==y.evidence_kind &&
                                           x.evidence_id==y.evidence_id;});
}
void EvidenceGroupPublication() {
  const std::vector<api::EngineEvidenceReference> input{
      {"identity",Id(4)},{"nil",api::EngineUuid{}},
      {"text",std::string("019d0000-0000-7000-8000-000000000004")},
      {"duplicate",std::string("first")},{"duplicate",std::string("second")},
      {std::string("kind\0suffix",11),std::string("value\0suffix",12)},
      {std::string(128,'k'),std::string(256,'v')}};
  const std::vector<api::EngineEvidenceReference> prior{
      {"prior",Id(9)},{"prior-text",std::string(128,'p')}};
  for(const std::string prefix:{std::string{},std::string("insert.locator"),std::string(128,'n')}) {
    auto expected=prior;
    for(const auto& item:input)expected.push_back({
        prefix.empty()?item.evidence_kind:prefix+"."+item.evidence_kind,item.evidence_id});
    auto output=prior;
    api::AppendApiEvidenceGroup(output,input,prefix);
    Check(SameEvidence(output,expected),"group append changed value type/bytes/order or kind namespace");
    bool complete=false;
    unsigned observed_faults=0;
    for(long allocation=0;allocation<128;++allocation) {
      auto fault_output=prior;
      const auto capacity=fault_output.capacity();
      try {
        fail_after=allocation;
        api::AppendApiEvidenceGroup(fault_output,input,prefix);
        fail_after=-1;complete=true;
        Check(SameEvidence(fault_output,expected),"group fault sweep success changed evidence");
        break;
      }catch(const std::bad_alloc&) {
        fail_after=-1;++faults;++observed_faults;
        Check(SameEvidence(fault_output,prior)&&fault_output.capacity()==capacity,
              "group allocation failure published partial evidence");
      }
    }
    Check(complete&&observed_faults>0,"group fault sweep failed to reach success");
  }
  for(unsigned position=0;position<16;++position)for(unsigned byte=0;byte<256;++byte) {
    auto id=Id(4);id.bytes[position]=byte;
    const std::vector<api::EngineEvidenceReference> source{{"uuid",id}};
    std::vector<api::EngineEvidenceReference> output;
    api::AppendApiEvidenceGroup(output,source,"source");
    const auto* value=std::get_if<api::EngineUuid>(&output[0].evidence_id);
    Check(output[0].evidence_kind=="source.uuid"&&value&&*value==id,
          "namespaced evidence changed UUID data bytes");
  }
  // Both source and namespace may borrow destination storage across growth.
  auto alias=input;
  const auto prefix=alias.back().evidence_kind;
  auto expected=input;
  for(const auto& item:input)expected.push_back({prefix+"."+item.evidence_kind,item.evidence_id});
  api::AppendApiEvidenceGroup(alias,alias,alias.back().evidence_kind);
  Check(SameEvidence(alias,expected),"self-append invalidated source or namespace");
  bool alias_complete=false;
  unsigned alias_faults=0;
  for(long allocation=0;allocation<128;++allocation) {
    auto output=input;
    const auto capacity=output.capacity();
    try {
      fail_after=allocation;
      api::AppendApiEvidenceGroup(output,output,output.back().evidence_kind);
      fail_after=-1;alias_complete=true;
      Check(SameEvidence(output,expected),"aliased fault sweep success changed evidence");
      break;
    }catch(const std::bad_alloc&) {
      fail_after=-1;++faults;++alias_faults;
      Check(SameEvidence(output,input)&&output.capacity()==capacity,
            "aliased group fault changed source/destination");
    }
  }
  Check(alias_complete&&alias_faults>0,"aliased fault sweep failed to reach success");
  auto unchanged=prior;
  fail_after=0;
  api::AppendApiEvidenceGroup(unchanged,{},"empty");
  fail_after=-1;
  Check(SameEvidence(unchanged,prior),"empty group changed destination");
}
}
int main() {
  EvidenceGroupPublication();
  // These are data UUIDs, not system identity admission: retain every byte,
  // including nil and older versions, without inventing a text shadow.
  for(unsigned position=0;position<16;++position)for(unsigned byte=0;byte<256;++byte) {
    auto id=Id(3);id.bytes[position]=byte;
    const auto value=api::ApiBehaviorValue(id);
    Check(Binary(value,id),"UUID value bytes changed or acquired a text shadow");
    Check(!value.isSqlNull() && value.state==api::EngineValueState::value,
          "UUID value state changed");
    Check(Unbound(value.descriptor),"convenience value fabricated bound descriptor authority");
  }
  Check(Binary(api::ApiBehaviorValue(api::EngineUuid{}),{}),"nil data UUID became absent or text");
  const std::string spelling="019e150f-0000-7000-8000-000000000003";
  const auto text=api::ApiBehaviorValue(spelling);
  Check(text.encoded_value==spelling && text.binary_value.empty() &&
        text.descriptor.canonical_type_name=="text","UUID-looking user TEXT was reinterpreted");
  api::EngineTypedValue bound;
  bound.descriptor.descriptor_uuid=Id(11);bound.descriptor.type_uuid=Id(12);
  bound.descriptor.datatype_descriptor_uuid=Id(13);bound.descriptor.datatype_descriptor_generation=87;
  bound.descriptor.collation_uuid=Id(14);bound.descriptor.charset_uuid=Id(15);
  bound.descriptor.canonical_type_name="fixture.bound.value";
  bound.binary_value={0,255,0,128,7};bound.encoded_value="source metadata";
  const auto row=api::ApiBehaviorRow({{"identity",Id(3)},{"text",spelling},
                                    {"literal","literal"},{"bound",bound}});
  Check(scratchbird::core::uuid::IsEngineIdentityUuid(row.requested_row_uuid),
        "result row identity is not a binary system UUIDv7");
  Check(row.fields.size()==4,"mixed result fields lost");
  Check(row.fields[0].first=="identity" && Binary(row.fields[0].second,Id(3)),"mixed UUID field changed");
  Check(row.fields[1].second.encoded_value==spelling && row.fields[1].second.binary_value.empty(),
        "mixed TEXT field was converted to UUID");
  Check(row.fields[2].second.encoded_value=="literal","literal text field changed");
  Check(row.fields[3].second.descriptor==bound.descriptor &&
        row.fields[3].second.binary_value==bound.binary_value &&
        row.fields[3].second.encoded_value==bound.encoded_value,"bound source descriptor/value changed");
  for(auto state:{api::EngineValueState::sql_null,api::EngineValueState::missing,
      api::EngineValueState::default_requested,api::EngineValueState::unknown,
      api::EngineValueState::error,api::EngineValueState::lob_handle,
      api::EngineValueState::protected_value}) {
    auto input=bound;input.setState(state);
    const auto r=api::ApiBehaviorRow({{"source",input}});
    Check(r.fields[0].second.state==state && r.fields[0].second.is_null==input.is_null &&
          r.fields[0].second.descriptor==input.descriptor && r.fields[0].second.binary_value==input.binary_value,
          "typed value state or descriptor overwritten");
  }
  std::vector<std::pair<std::string,std::string>> old_fields{{"object_uuid",spelling},{"empty",""}};
  const auto strings=api::ApiBehaviorRow(old_fields);
  Check(strings.fields.size()==2 && strings.fields[0].second.encoded_value==spelling &&
        strings.fields[0].second.binary_value.empty(),"explicit TEXT vector inferred an identity");
  api::EngineApiResult result;
  result.ok=false;result.operation_id="unchanged";
  result.result_shape.result_kind="prior-kind";result.result_shape.rows.push_back(row);
  api::AddApiBehaviorRow(&result,old_fields);
  Check(!result.ok && result.operation_id=="unchanged" && result.result_shape.rows.size()==2 &&
        result.result_shape.result_kind=="api_behavior_rows","append changed outcome or lost prior row");
  bool completed=false;
  for(long allocation=0;allocation<256;++allocation) {
    auto output=result;
    output.result_shape.result_kind="a preexisting result kind that requires allocation";
    const auto before=output.result_shape.result_kind;
    try {
      fail_after=allocation;
      api::AddApiBehaviorRow(&output,{{"binary identity source field",Id(3)},
          {"long source text field",std::string(128,'x')},{"bound source field",bound}});
      fail_after=-1;completed=true;
      Check(output.result_shape.rows.size()==3 && Binary(output.result_shape.rows.back().fields[0].second,Id(3)),
            "successful append lost binary field");
      break;
    } catch(const std::bad_alloc&) {
      fail_after=-1;++faults;
      Check(output.result_shape.result_kind==before && output.result_shape.rows.size()==2 &&
            output.result_shape.rows[0].requested_row_uuid==row.requested_row_uuid &&
            output.result_shape.rows[0].fields[3].second.descriptor==bound.descriptor &&
            !output.ok && output.operation_id=="unchanged","failed append changed caller result");
    }
  }
  Check(completed && faults>0,"allocation-fault coverage did not reach success");
  api::EngineApiResult evidence_result;
  evidence_result.operation_id="source-outcome";
  for(unsigned position=0;position<16;++position)for(unsigned byte=0;byte<256;++byte){
    auto id=Id(3);id.bytes[position]=byte;
    api::AddApiBehaviorEvidence(&evidence_result,"identity",id);
    const auto& reference=evidence_result.evidence.back();
    const auto* binary=std::get_if<api::EngineUuid>(&reference.evidence_id);
    Check(reference.evidence_kind=="identity" && binary && *binary==id,
          "evidence reference lost binary bytes or became text");
  }
  api::AddApiBehaviorEvidence(&evidence_result,"text",spelling);
  Check(std::get<std::string>(evidence_result.evidence.back().evidence_id)==spelling,
        "text evidence spelling reinterpreted as identity");
  api::AddApiBehaviorEvidence(&evidence_result,"absent_predecessor",api::EngineUuid{});
  Check(std::get<api::EngineUuid>(evidence_result.evidence.back().evidence_id).is_nil(),
        "explicit nil reference was replaced or stringified");
  completed=false;
  for(long allocation=0;allocation<64;++allocation){
    api::EngineApiResult output;output.operation_id="source-outcome";
    api::AddApiBehaviorEvidence(&output,"prior",Id(4));
    try {
      fail_after=allocation;
      api::AddApiBehaviorEvidence(&output,std::string(128,'k'),Id(5));
      fail_after=-1;completed=true;
      Check(output.evidence.size()==2 && std::get<api::EngineUuid>(output.evidence.back().evidence_id)==Id(5),
            "successful evidence append lost identity");break;
    }catch(const std::bad_alloc&){
      fail_after=-1;++faults;
      Check(output.evidence.size()==1 && std::get<api::EngineUuid>(output.evidence[0].evidence_id)==Id(4) &&
            !output.ok && output.operation_id=="source-outcome","failed evidence append changed source result");
    }
  }
  Check(completed && !evidence_result.ok && evidence_result.operation_id=="source-outcome",
        "evidence helper invented success or fault sweep incomplete");
  api::AddApiBehaviorEvidence(nullptr,"ignored",Id(1));
  std::cout<<"api_behavior_binary_rows checks="<<checks<<" faults="<<faults<<" failures="<<failures<<'\n';
  return failures?1:0;
}
