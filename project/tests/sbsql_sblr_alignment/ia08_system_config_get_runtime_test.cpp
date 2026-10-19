// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/sblr_system_config_get_runtime.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace {
namespace s=scratchbird::engine::sblr;
using Bytes=std::vector<std::uint8_t>;
void Check(bool good,const char* message){if(!good)throw std::runtime_error(message);}
s::SystemConfigGetUuid Id(unsigned seed){
  s::SystemConfigGetUuid value;
  for(unsigned i=0;i<16;++i)value[i]=static_cast<std::uint8_t>(seed+7*i);
  value[6]=(value[6]&15)|0x70;value[8]=(value[8]&63)|0x80;return value;
}
void Put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned width){
  for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
void Identity(Bytes& bytes,std::size_t at,const s::SystemConfigGetUuid& value){std::copy(value.begin(),value.end(),bytes.begin()+at);}
Bytes Header(const char* magic,std::size_t size){
  Bytes bytes(size,0);std::copy_n(magic,4,bytes.begin());Put(bytes,4,1,2);Put(bytes,6,8,2);return bytes;
}
// Independent existing layout; distinct bytes detect shifts hidden by the old
// one-byte identities and assertions that checked only decoder acceptance.
void Descriptor(){
  s::SblrSystemConfigGetDescriptorV1 value;
  value.operation=Id(1);value.receipt=Id(2);value.scope=Id(3);value.snapshot=Id(4);
  value.projection=Id(5);value.redaction_policy=Id(6);value.security_context=Id(7);
  value.transaction=Id(8);value.route_evidence=Id(9);
  value.scope_generation=0x1234567890abcdefULL;value.snapshot_generation=0x2233445566778899ULL;
  value.policy_generation=0x9988776655443322ULL;
  auto expected=Header("SLGC",256);
  Identity(expected,8,value.operation);Identity(expected,24,value.receipt);Identity(expected,40,value.scope);
  Put(expected,56,value.scope_generation,8);Identity(expected,64,value.snapshot);Put(expected,80,value.snapshot_generation,8);
  Identity(expected,88,value.projection);Identity(expected,104,value.redaction_policy);Put(expected,120,value.policy_generation,8);
  Identity(expected,128,value.security_context);Identity(expected,144,value.transaction);Identity(expected,160,value.route_evidence);
  Check(s::EncodeSblrSystemConfigGetDescriptorV1(value)==expected,"descriptor wire bytes changed");
  s::SblrSystemConfigGetDescriptorV1 decoded;
  Check(s::DecodeSblrSystemConfigGetDescriptorV1(expected.data(),expected.size(),&decoded,nullptr),"descriptor rejected");
  Check(decoded.operation==value.operation&&decoded.receipt==value.receipt&&decoded.scope==value.scope&&
      decoded.snapshot==value.snapshot&&decoded.projection==value.projection&&decoded.redaction_policy==value.redaction_policy&&
      decoded.security_context==value.security_context&&decoded.transaction==value.transaction&&decoded.route_evidence==value.route_evidence&&
      decoded.scope_generation==value.scope_generation&&decoded.snapshot_generation==value.snapshot_generation&&
      decoded.policy_generation==value.policy_generation,"descriptor identity or generation shifted on decode");
  Check(s::EncodeSblrSystemConfigGetDescriptorV1(decoded)==expected,"decoded descriptor does not preserve the request");
  for(std::size_t n=0;n<expected.size();++n)
    Check(!s::DecodeSblrSystemConfigGetDescriptorV1(expected.data(),n,&decoded,nullptr),"truncated descriptor accepted");
  expected.push_back(0);Check(!s::DecodeSblrSystemConfigGetDescriptorV1(expected.data(),expected.size(),&decoded,nullptr),"trailing byte accepted");
}
void RequestAndResult(){
  const s::SblrSystemConfigGetRequestV1 request{Id(10),Id(11)};
  auto expected=Header("SCGQ",64);Put(expected,8,7543,4);Put(expected,12,530,4);
  Identity(expected,16,request.operation);Identity(expected,32,request.receipt);
  Check(s::EncodeSblrSystemConfigGetRequestV1(request)==expected,"request layout mismatch");
  s::SblrSystemConfigGetRequestV1 decoded;
  Check(s::DecodeSblrSystemConfigGetRequestV1(expected.data(),expected.size(),&decoded,nullptr)&&
      decoded.operation==request.operation&&decoded.receipt==request.receipt,"request identity mismatch");
  expected[4]=2;Check(!s::DecodeSblrSystemConfigGetRequestV1(expected.data(),expected.size(),&decoded,nullptr),"unknown version accepted");
  s::SblrSystemConfigGetResultV1 value;
  value.operation=Id(12);value.scope=Id(13);value.snapshot=Id(14);value.evidence=Id(15);value.recovery_evidence=Id(16);
  value.scope_generation=0x0102030405060708ULL;value.snapshot_generation=0x8877665544332211ULL;
  value.redaction_class=1;value.status=1;value.projection_length=value.projection.size();
  for(std::size_t i=0;i<value.projection.size();++i)value.projection[i]=static_cast<std::uint8_t>(13*i);
  expected=Header("SLGR",512);Identity(expected,8,value.operation);Identity(expected,24,value.scope);
  Put(expected,40,value.scope_generation,8);Identity(expected,48,value.snapshot);Put(expected,64,value.snapshot_generation,8);
  Put(expected,72,value.redaction_class,4);Put(expected,76,value.status,4);Identity(expected,80,value.evidence);
  Identity(expected,128,value.recovery_evidence);Put(expected,144,value.projection_length,4);
  std::copy(value.projection.begin(),value.projection.end(),expected.begin()+148);
  Check(s::EncodeSblrSystemConfigGetResultV1(value)==expected,"result layout mismatch");
  s::SblrSystemConfigGetResultV1 result;
  Check(s::DecodeSblrSystemConfigGetResultV1(expected.data(),expected.size(),&result,nullptr)&&
      result.operation==value.operation&&result.scope==value.scope&&result.snapshot==value.snapshot&&
      result.evidence==value.evidence&&result.recovery_evidence==value.recovery_evidence&&
      result.scope_generation==value.scope_generation&&result.snapshot_generation==value.snapshot_generation&&
      result.redaction_class==value.redaction_class&&result.status==value.status&&
      result.projection_length==value.projection_length&&result.projection==value.projection,"result data mismatch");
  Check(s::EncodeSblrSystemConfigGetResultV1(result)==expected,"result does not round trip");
}
}
int main(){try{Descriptor();RequestAndResult();return 0;}
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
