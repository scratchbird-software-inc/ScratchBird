#include "sblr_function_call_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr { namespace {
void put(std::vector<std::uint8_t>& out, std::uint64_t value, std::size_t bytes) { for (std::size_t i=0;i<bytes;++i) out.push_back(static_cast<std::uint8_t>(value>>(8*i))); }
std::uint64_t get(const std::uint8_t* bytes, std::size_t count) { std::uint64_t value=0; for(std::size_t i=0;i<count;++i) value|=std::uint64_t(bytes[i])<<(8*i); return value; }
template<class T> bool nonzero(const T& value) { return std::any_of(value.begin(),value.end(),[](auto byte){return byte!=0;}); }
std::vector<std::uint8_t> header(const char* magic,std::size_t bytes) { std::vector<std::uint8_t> out(magic,magic+4);put(out,1,2);put(out,bytes,2);put(out,bytes,4);put(out,0,4);return out; }
bool valid_header(const std::uint8_t* bytes,std::size_t count,const char* magic,std::size_t expected) { return bytes&&count==expected&&std::equal(bytes,bytes+4,magic)&&get(bytes+4,2)==1&&get(bytes+6,2)==expected&&get(bytes+8,4)==expected&&std::all_of(bytes+12,bytes+16,[](auto b){return b==0;}); }
FunctionCallSha digest(const char* domain,const std::uint8_t* bytes,std::size_t count) { std::vector<std::uint8_t> material(domain,domain+std::strlen(domain));material.insert(material.end(),bytes,bytes+count);return scratchbird::core::hash::ComputeSha256Digest(material).digest; }
}

std::vector<std::uint8_t> EncodeSblrFunctionCallRequestV1(const SblrFunctionCallRequestV1& value) {
    if(!nonzero(value.receipt)||value.occurrence==0||value.function_occurrence==0)return {};
    auto out=header("FCRQ",64);out.insert(out.end(),value.receipt.begin(),value.receipt.end());put(out,value.occurrence,8);put(out,value.function_occurrence,4);out.insert(out.end(),20,0);return out;
}
bool DecodeSblrFunctionCallRequestV1(const std::uint8_t* bytes,std::size_t count,SblrFunctionCallRequestV1* out,std::string* detail) {
    if(!out||!valid_header(bytes,count,"FCRQ",64)||std::any_of(bytes+44,bytes+64,[](auto b){return b!=0;})){if(detail)*detail="FCRQ invalid";return false;}
    SblrFunctionCallRequestV1 value;std::copy_n(bytes+16,16,value.receipt.begin());value.occurrence=get(bytes+32,8);value.function_occurrence=get(bytes+40,4);if(EncodeSblrFunctionCallRequestV1(value).empty())return false;*out=value;return true;
}
std::vector<std::uint8_t> EncodeSblrFunctionCallDescriptorV1(const SblrFunctionCallDescriptorV1& value,bool operand) {
    if(!nonzero(value.canonical_body)||value.availability_generation==0)return {};
    auto out=header(operand?"FCDO":"FCDD",424);out.insert(out.end(),value.canonical_body.begin(),value.canonical_body.end());auto evidence=digest("ScratchBird.SblrFunctionCallDescriptor.V1",out.data()+16,368);if(nonzero(value.evidence)&&value.evidence!=evidence)return{};out.insert(out.end(),evidence.begin(),evidence.end());put(out,value.availability_generation,8);return out;
}
bool DecodeSblrFunctionCallDescriptorV1(const std::uint8_t* bytes,std::size_t count,SblrFunctionCallDescriptorV1* out,std::string* detail,bool operand) {
    if(!out||!valid_header(bytes,count,operand?"FCDO":"FCDD",424)){if(detail)*detail="function call descriptor invalid";return false;}
    SblrFunctionCallDescriptorV1 value;std::copy_n(bytes+16,368,value.canonical_body.begin());std::copy_n(bytes+384,32,value.evidence.begin());value.availability_generation=get(bytes+416,8);if(EncodeSblrFunctionCallDescriptorV1(value,operand).empty())return false;*out=value;return true;
}
std::vector<std::uint8_t> EncodeSblrFunctionCallResultV1(const SblrFunctionCallResultV1& value) {
    if(!nonzero(value.canonical_body)||value.availability_generation==0)return{};
    const auto state=value.canonical_body[40];const auto length=get(value.canonical_body.data()+44,4);if(state>2||(state==0&&length!=0)||(state==1&&length>96)||(state==2&&length<=96))return{};
    if(state==0&&std::any_of(value.canonical_body.begin()+48,value.canonical_body.begin()+176,[](auto b){return b!=0;}))return{};
    if(state==1&&std::any_of(value.canonical_body.begin()+48+length,value.canonical_body.begin()+144,[](auto b){return b!=0;}))return{};
    auto out=header("FCRS",256);out.insert(out.end(),value.canonical_body.begin(),value.canonical_body.end());auto evidence=digest("ScratchBird.SblrFunctionCallExecutorEvidence.V1",out.data()+16,176);if(nonzero(value.executor_evidence)&&value.executor_evidence!=evidence)return{};out.insert(out.end(),evidence.begin(),evidence.end());put(out,value.availability_generation,8);out.insert(out.end(),24,0);return out;
}
bool DecodeSblrFunctionCallResultV1(const std::uint8_t* bytes,std::size_t count,SblrFunctionCallResultV1* out,std::string* detail) {
    if(!out||!valid_header(bytes,count,"FCRS",256)||std::any_of(bytes+232,bytes+256,[](auto b){return b!=0;})){if(detail)*detail="FCRS invalid";return false;}
    SblrFunctionCallResultV1 value;std::copy_n(bytes+16,176,value.canonical_body.begin());std::copy_n(bytes+192,32,value.executor_evidence.begin());value.availability_generation=get(bytes+224,8);if(EncodeSblrFunctionCallResultV1(value).empty())return false;*out=value;return true;
}
}
