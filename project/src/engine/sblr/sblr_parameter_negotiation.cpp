// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_parameter_runtime.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <limits>
#include <string_view>
namespace scratchbird::engine::sblr {namespace {
using Bytes=std::vector<std::uint8_t>;void P16(Bytes*o,std::uint16_t v){o->push_back(v);o->push_back(v>>8);}void P32(Bytes*o,std::uint32_t v){for(unsigned i=0;i<4;++i)o->push_back(v>>(8*i));}void P64(Bytes*o,std::uint64_t v){for(unsigned i=0;i<8;++i)o->push_back(v>>(8*i));}std::uint16_t U16(const std::uint8_t*p){return p[0]|std::uint16_t(p[1])<<8;}std::uint32_t U32(const std::uint8_t*p){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(p[i])<<(8*i);return v;}std::uint64_t U64(const std::uint8_t*p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}template<class A>bool Nz(const A&a){return std::any_of(a.begin(),a.end(),[](auto b){return b;});}template<class A>bool Pair(const A&a,std::uint64_t g){return Nz(a)==(g!=0);}std::array<std::uint8_t,32> Hash(std::string_view domain,const Bytes&b){Bytes m(domain.begin(),domain.end());m.insert(m.end(),b.begin(),b.end());auto d=core::hash::ComputeSha256Digest(m);return d.ok()?d.digest:std::array<std::uint8_t,32>{};}
}
std::array<std::uint8_t,32> ComputeSblrParameterDemandSha256V1(const std::vector<SblrParameterDemandV1>&v){if(v.empty())return{};Bytes b;P32(&b,v.size());std::uint64_t prior=0;std::uint32_t marker=1;for(const auto&x:v){if(!x.occurrence_id||x.occurrence_id<=prior||x.marker_ordinal!=marker++||x.requested_direction<1||x.requested_direction>3||x.nullable_demand>1)return{};P64(&b,x.occurrence_id);P32(&b,x.marker_ordinal);P16(&b,x.context_code);b.push_back(x.requested_direction);b.push_back(x.nullable_demand);P64(&b,0);prior=x.occurrence_id;}return Hash("ScratchBird.SblrParameterDemandSequence.V1",b);}
std::array<std::uint8_t,32> ComputeSblrParameterMappingSha256V1(const std::vector<SblrParameterMappingV1>&v){if(v.empty())return{};Bytes b;P32(&b,v.size());std::uint64_t prior=0;std::uint32_t slot=0;for(const auto&x:v){if(!x.occurrence_id||x.occurrence_id<=prior||x.slot_ordinal!=slot++||!Nz(x.slot_uuid)||!Nz(x.datatype_descriptor_uuid)||!Nz(x.datatype_type_uuid)||!x.datatype_descriptor_generation||x.direction<1||x.direction>3||x.nullable>1)return{};P64(&b,x.occurrence_id);P32(&b,x.slot_ordinal);b.insert(b.end(),x.slot_uuid.begin(),x.slot_uuid.end());b.insert(b.end(),x.datatype_descriptor_uuid.begin(),x.datatype_descriptor_uuid.end());b.insert(b.end(),x.datatype_type_uuid.begin(),x.datatype_type_uuid.end());P64(&b,x.datatype_descriptor_generation);b.push_back(x.direction);b.push_back(x.nullable);P16(&b,0);prior=x.occurrence_id;}return Hash("ScratchBird.SblrParameterDescriptorMappings.V1",b);}
Bytes EncodeSblrParameterNegotiateRequestV1(const SblrParameterNegotiateRequestV1&v){if(!Nz(v.preliminary_receipt_uuid)||!v.catalog_generation||!Nz(v.mga_snapshot_uuid)||v.demands.empty()||v.demands.size()>4096||ComputeSblrParameterDemandSha256V1(v.demands)!=v.demand_sha256)return{};Bytes o;o.insert(o.end(),{'S','B','P','R'});P16(&o,1);P16(&o,112);P32(&o,112+24*v.demands.size());P32(&o,0);o.insert(o.end(),v.preliminary_receipt_uuid.begin(),v.preliminary_receipt_uuid.end());P64(&o,v.catalog_generation);P64(&o,v.security_epoch);P64(&o,v.resource_epoch);o.insert(o.end(),v.mga_snapshot_uuid.begin(),v.mga_snapshot_uuid.end());P32(&o,v.demands.size());P32(&o,24);o.insert(o.end(),v.demand_sha256.begin(),v.demand_sha256.end());for(const auto&x:v.demands){P64(&o,x.occurrence_id);P32(&o,x.marker_ordinal);P16(&o,x.context_code);o.push_back(x.requested_direction);o.push_back(x.nullable_demand);P64(&o,0);}return o;}
bool DecodeSblrParameterNegotiateRequestV1(const std::uint8_t*b,std::size_t n,SblrParameterNegotiateRequestV1*out,std::string*d){if(!b||!out||n<112||n>98416||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPR"))||U16(b+4)!=1||U16(b+6)!=112||U32(b+8)!=n||U32(b+12)||U32(b+76)!=24){if(d)*d="SBPR header invalid";return false;}auto c=U32(b+72);if(c>4096||112+std::size_t(c)*24!=n){if(d)*d="SBPR extent invalid";return false;}SblrParameterNegotiateRequestV1 v;std::copy_n(b+16,16,v.preliminary_receipt_uuid.begin());v.catalog_generation=U64(b+32);v.security_epoch=U64(b+40);v.resource_epoch=U64(b+48);std::copy_n(b+56,16,v.mga_snapshot_uuid.begin());std::copy_n(b+80,32,v.demand_sha256.begin());for(std::uint32_t i=0;i<c;++i){auto o=112+i*24;SblrParameterDemandV1 x;x.occurrence_id=U64(b+o);x.marker_ordinal=U32(b+o+8);x.context_code=U16(b+o+12);x.requested_direction=b[o+14];x.nullable_demand=b[o+15];if(U64(b+o+16)){if(d)*d="SBPR reserve invalid";return false;}v.demands.push_back(x);}if(EncodeSblrParameterNegotiateRequestV1(v)!=Bytes(b,b+n)){if(d)*d="SBPR reencode differs";return false;}*out=std::move(v);return true;}
Bytes EncodeSblrParameterNegotiateResultV1(const SblrParameterNegotiateResultV1&v){if(!Nz(v.preliminary_receipt_uuid)||!Nz(v.parameter_set_descriptor_uuid)||!v.descriptor_generation||!Nz(v.execution_uuid)||v.mappings.size()>4096||ComputeSblrParameterMappingSha256V1(v.mappings)!=v.mapping_sha256)return{};Bytes o;o.insert(o.end(),{'S','B','P','G'});P16(&o,1);P16(&o,112);P32(&o,112+72*v.mappings.size());P32(&o,0);o.insert(o.end(),v.preliminary_receipt_uuid.begin(),v.preliminary_receipt_uuid.end());P32(&o,v.mappings.size());P32(&o,72);o.insert(o.end(),v.parameter_set_descriptor_uuid.begin(),v.parameter_set_descriptor_uuid.end());P64(&o,v.descriptor_generation);o.insert(o.end(),v.execution_uuid.begin(),v.execution_uuid.end());o.insert(o.end(),v.mapping_sha256.begin(),v.mapping_sha256.end());for(const auto&x:v.mappings){P64(&o,x.occurrence_id);P32(&o,x.slot_ordinal);o.insert(o.end(),x.slot_uuid.begin(),x.slot_uuid.end());o.insert(o.end(),x.datatype_descriptor_uuid.begin(),x.datatype_descriptor_uuid.end());o.insert(o.end(),x.datatype_type_uuid.begin(),x.datatype_type_uuid.end());P64(&o,x.datatype_descriptor_generation);o.push_back(x.direction);o.push_back(x.nullable);P16(&o,0);}return o;}
bool DecodeSblrParameterNegotiateResultV1(const std::uint8_t*b,std::size_t n,SblrParameterNegotiateResultV1*out,std::string*d){if(!b||!out||n<112||n>295024||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPG"))||U16(b+4)!=1||U16(b+6)!=112||U32(b+8)!=n||U32(b+12)||U32(b+36)!=72){if(d)*d="SBPG header invalid";return false;}auto c=U32(b+32);if(c>4096||112+std::size_t(c)*72!=n){if(d)*d="SBPG extent invalid";return false;}SblrParameterNegotiateResultV1 v;std::copy_n(b+16,16,v.preliminary_receipt_uuid.begin());std::copy_n(b+40,16,v.parameter_set_descriptor_uuid.begin());v.descriptor_generation=U64(b+56);std::copy_n(b+64,16,v.execution_uuid.begin());std::copy_n(b+80,32,v.mapping_sha256.begin());for(std::uint32_t i=0;i<c;++i){auto o=112+i*72;SblrParameterMappingV1 x;x.occurrence_id=U64(b+o);x.slot_ordinal=U32(b+o+8);std::copy_n(b+o+12,16,x.slot_uuid.begin());std::copy_n(b+o+28,16,x.datatype_descriptor_uuid.begin());std::copy_n(b+o+44,16,x.datatype_type_uuid.begin());x.datatype_descriptor_generation=U64(b+o+60);x.direction=b[o+68];x.nullable=b[o+69];if(U16(b+o+70)){if(d)*d="SBPG reserve invalid";return false;}v.mappings.push_back(x);}if(EncodeSblrParameterNegotiateResultV1(v)!=Bytes(b,b+n)){if(d)*d="SBPG reencode differs";return false;}*out=std::move(v);return true;}
Bytes EncodeSblrParameterFinalizeRequestV1(const SblrParameterFinalizeRequestV1&v){auto table=DecodeSblrParameterNodeTableV1(v.canonical_sbpn.data(),v.canonical_sbpn.size());if(!Nz(v.preliminary_receipt_uuid)||!Nz(v.parameter_set_descriptor_uuid)||!v.descriptor_generation||!Nz(v.execution_uuid)||!table.ok||v.canonical_sbpn.size()>426016)return{};Bytes material("ScratchBird.SblrParameterNodeTable.V1","ScratchBird.SblrParameterNodeTable.V1"+37);material.insert(material.end(),v.canonical_sbpn.begin(),v.canonical_sbpn.end());auto sha=core::hash::ComputeSha256Digest(material);if(!sha.ok()||sha.digest!=v.sbpn_sha256)return{};Bytes o;o.insert(o.end(),{'S','B','P','F'});P16(&o,1);P16(&o,176);P32(&o,176+v.canonical_sbpn.size());P32(&o,0);o.insert(o.end(),v.preliminary_receipt_uuid.begin(),v.preliminary_receipt_uuid.end());o.insert(o.end(),v.parameter_set_descriptor_uuid.begin(),v.parameter_set_descriptor_uuid.end());P64(&o,v.descriptor_generation);o.insert(o.end(),v.execution_uuid.begin(),v.execution_uuid.end());o.insert(o.end(),v.demand_sha256.begin(),v.demand_sha256.end());o.insert(o.end(),v.mapping_sha256.begin(),v.mapping_sha256.end());o.insert(o.end(),v.sbpn_sha256.begin(),v.sbpn_sha256.end());P32(&o,v.canonical_sbpn.size());P32(&o,0);o.insert(o.end(),v.canonical_sbpn.begin(),v.canonical_sbpn.end());return o;}
bool DecodeSblrParameterFinalizeRequestV1(const std::uint8_t*b,std::size_t n,SblrParameterFinalizeRequestV1*out,std::string*d){if(!b||!out||n<176||n>426192||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPF"))||U16(b+4)!=1||U16(b+6)!=176||U32(b+8)!=n||U32(b+12)||U32(b+172)||U32(b+168)!=n-176){if(d)*d="SBPF header invalid";return false;}SblrParameterFinalizeRequestV1 v;std::copy_n(b+16,16,v.preliminary_receipt_uuid.begin());std::copy_n(b+32,16,v.parameter_set_descriptor_uuid.begin());v.descriptor_generation=U64(b+48);std::copy_n(b+56,16,v.execution_uuid.begin());std::copy_n(b+72,32,v.demand_sha256.begin());std::copy_n(b+104,32,v.mapping_sha256.begin());std::copy_n(b+136,32,v.sbpn_sha256.begin());v.canonical_sbpn.assign(b+176,b+n);if(EncodeSblrParameterFinalizeRequestV1(v)!=Bytes(b,b+n)){if(d)*d="SBPF reencode differs";return false;}*out=std::move(v);return true;}
Bytes EncodeSblrParameterAdmissionV1(SblrParameterAdmissionV1*v){if(!v||!Nz(v->final_receipt_uuid)||!Nz(v->admission_token_uuid)||!Nz(v->parameter_set_descriptor_uuid)||!v->descriptor_generation||!Nz(v->execution_uuid)||!Pair(v->prepared_uuid,v->prepared_generation)||!Pair(v->batch_uuid,v->batch_generation)||!Pair(v->dynamic_uuid,v->dynamic_generation)||(Nz(v->prepared_uuid)&&Nz(v->dynamic_uuid)))return{};Bytes o;o.insert(o.end(),{'S','B','P','A'});P16(&o,1);P16(&o,192);P32(&o,192);P32(&o,0);o.insert(o.end(),v->final_receipt_uuid.begin(),v->final_receipt_uuid.end());o.insert(o.end(),v->admission_token_uuid.begin(),v->admission_token_uuid.end());o.insert(o.end(),v->parameter_set_descriptor_uuid.begin(),v->parameter_set_descriptor_uuid.end());P64(&o,v->descriptor_generation);o.insert(o.end(),v->execution_uuid.begin(),v->execution_uuid.end());for(const auto&pair:{std::pair{v->prepared_uuid,v->prepared_generation},std::pair{v->batch_uuid,v->batch_generation},std::pair{v->dynamic_uuid,v->dynamic_generation}}){o.insert(o.end(),pair.first.begin(),pair.first.end());P64(&o,pair.second);}auto binding=Hash("ScratchBird.SblrParameterBinding.V1",Bytes(o.begin()+16,o.end()));v->binding_sha256=binding;o.insert(o.end(),binding.begin(),binding.end());return o;}
bool DecodeSblrParameterAdmissionV1(const std::uint8_t*b,std::size_t n,SblrParameterAdmissionV1*out,std::string*d){if(!b||!out||n!=192||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPA"))||U16(b+4)!=1||U16(b+6)!=192||U32(b+8)!=192||U32(b+12)){if(d)*d="SBPA header invalid";return false;}SblrParameterAdmissionV1 v;std::copy_n(b+16,16,v.final_receipt_uuid.begin());std::copy_n(b+32,16,v.admission_token_uuid.begin());std::copy_n(b+48,16,v.parameter_set_descriptor_uuid.begin());v.descriptor_generation=U64(b+64);std::copy_n(b+72,16,v.execution_uuid.begin());std::copy_n(b+88,16,v.prepared_uuid.begin());v.prepared_generation=U64(b+104);std::copy_n(b+112,16,v.batch_uuid.begin());v.batch_generation=U64(b+128);std::copy_n(b+136,16,v.dynamic_uuid.begin());v.dynamic_generation=U64(b+152);std::copy_n(b+160,32,v.binding_sha256.begin());if(EncodeSblrParameterAdmissionV1(&v)!=Bytes(b,b+n)){if(d)*d="SBPA binding invalid";return false;}*out=std::move(v);return true;}

SblrParameterWirePrevalidationV1 PrevalidateSblrParameterNegotiateRequestV1(
    const std::uint8_t* b, std::size_t n) {
  if (!b || n < 112 || !std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPR")) ||
      U16(b+4)!=1 || U16(b+6)!=112 || U32(b+8)!=n || U32(b+12)!=0 ||
      U32(b+76)!=24||!std::any_of(b+16,b+32,[](auto x){return x!=0;})||
      U64(b+32)==0||!std::any_of(b+56,b+72,[](auto x){return x!=0;})||
      !std::any_of(b+80,b+112,[](auto x){return x!=0;}))
    return SblrParameterWirePrevalidationV1::operand_invalid;
  const auto count=U32(b+72);
  if(count==0 || count>(std::numeric_limits<std::size_t>::max()-112)/24 ||
     112+std::size_t(count)*24!=n)
    return SblrParameterWirePrevalidationV1::operand_invalid;
  std::uint64_t prior=0;std::uint32_t marker=1;
  for(std::uint32_t i=0;i<count;++i){const auto o=112+std::size_t(i)*24;
    const auto occurrence=U64(b+o);const auto direction=b[o+14];
    if(!occurrence||occurrence<=prior||U32(b+o+8)!=marker++||U16(b+o+12)!=1||
       direction!=1||b[o+15]>1||U64(b+o+16)!=0)
      return SblrParameterWirePrevalidationV1::operand_invalid;
    prior=occurrence;}
  return count>4096||n>98416?SblrParameterWirePrevalidationV1::resource_exceeded:
      SblrParameterWirePrevalidationV1::ok;
}

SblrParameterWirePrevalidationV1 PrevalidateSblrParameterNegotiateResultV1(
    const std::uint8_t* b, std::size_t n) {
  if(!b||n<112||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPG"))||
     U16(b+4)!=1||U16(b+6)!=112||U32(b+8)!=n||U32(b+12)!=0||U32(b+36)!=72||
     !std::any_of(b+16,b+32,[](auto x){return x!=0;})||
     !std::any_of(b+40,b+56,[](auto x){return x!=0;})||U64(b+56)==0||
     !std::any_of(b+64,b+80,[](auto x){return x!=0;})||
     !std::any_of(b+80,b+112,[](auto x){return x!=0;}))
    return SblrParameterWirePrevalidationV1::operand_invalid;
  const auto count=U32(b+32);
  if(count==0||count>(std::numeric_limits<std::size_t>::max()-112)/72||
     112+std::size_t(count)*72!=n)
    return SblrParameterWirePrevalidationV1::operand_invalid;
  std::uint64_t prior=0;
  for(std::uint32_t i=0;i<count;++i){const auto o=112+std::size_t(i)*72;
    const auto occurrence=U64(b+o);const auto direction=b[o+68];
    if(!occurrence||occurrence<=prior||U32(b+o+8)!=i||
       !std::any_of(b+o+12,b+o+28,[](auto x){return x!=0;})||
       !std::any_of(b+o+28,b+o+44,[](auto x){return x!=0;})||
       !std::any_of(b+o+44,b+o+60,[](auto x){return x!=0;})||U64(b+o+60)==0||
       direction!=1||b[o+69]>1||U16(b+o+70)!=0)
      return SblrParameterWirePrevalidationV1::operand_invalid;
    prior=occurrence;}
  return count>4096||n>295024?SblrParameterWirePrevalidationV1::resource_exceeded:
      SblrParameterWirePrevalidationV1::ok;
}

SblrParameterWirePrevalidationV1 PrevalidateSblrParameterFinalizeRequestV1(
    const std::uint8_t* b, std::size_t n) {
  if(!b||n<176||!std::equal(b,b+4,reinterpret_cast<const std::uint8_t*>("SBPF"))||
     U16(b+4)!=1||U16(b+6)!=176||U32(b+8)!=n||U32(b+12)!=0||U32(b+172)!=0||
     U32(b+168)!=n-176)
    return SblrParameterWirePrevalidationV1::operand_invalid;
  if(!std::any_of(b+16,b+32,[](auto x){return x!=0;})||
     !std::any_of(b+32,b+48,[](auto x){return x!=0;})||U64(b+48)==0||
     !std::any_of(b+56,b+72,[](auto x){return x!=0;})||
     !std::any_of(b+72,b+104,[](auto x){return x!=0;})||
     !std::any_of(b+104,b+136,[](auto x){return x!=0;})||
     !std::any_of(b+136,b+168,[](auto x){return x!=0;}))
    return SblrParameterWirePrevalidationV1::operand_invalid;
  const auto* table=b+176;const auto size=n-176;
  if(size<32||!std::equal(table,table+4,reinterpret_cast<const std::uint8_t*>("SBPN"))||
     U16(table+4)!=1||U16(table+6)!=32||U32(table+12)!=0||U64(table+16)!=size||
     U64(table+24)!=32)
    return SblrParameterWirePrevalidationV1::operand_invalid;
  const auto count=U32(table+8);
  if(count==0||count>(std::numeric_limits<std::size_t>::max()-32)/104||
     32+std::size_t(count)*104!=size)
    return SblrParameterWirePrevalidationV1::operand_invalid;
  std::uint64_t prior_node=0;
  for(std::uint32_t i=0;i<count;++i){const auto o=32+std::size_t(i)*104;
    if(U32(table+o)!=104||U64(table+o+4)<=prior_node||U32(table+o+12)!=i+1||
       !std::any_of(table+o+20,table+o+36,[](auto x){return x!=0;})||
       U64(table+o+36)==0||
       !std::any_of(table+o+44,table+o+60,[](auto x){return x!=0;})||
       U64(table+o+60)==0||
       U16(table+o+68)!=19||!std::equal(table+o+70,table+o+89,
           reinterpret_cast<const std::uint8_t*>("engine.op.parameter"))||
       U16(table+o+89)!=11||!std::equal(table+o+91,table+o+102,
           reinterpret_cast<const std::uint8_t*>("typed_value"))||U16(table+o+102)!=1)
      return SblrParameterWirePrevalidationV1::operand_invalid;
    prior_node=U64(table+o+4);}
  return count>4096||size>426016||n>426192?
      SblrParameterWirePrevalidationV1::resource_exceeded:
      SblrParameterWirePrevalidationV1::ok;
}
}
