// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_variable_runtime.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace scratchbird::engine::sblr {
namespace {
void P16(std::vector<std::uint8_t>* o, std::uint16_t v) { o->push_back(v); o->push_back(v >> 8); }
void P32(std::vector<std::uint8_t>* o, std::uint32_t v) { for (unsigned i=0;i<4;++i) o->push_back(v>>(8*i)); }
void P64(std::vector<std::uint8_t>* o, std::uint64_t v) { for (unsigned i=0;i<8;++i) o->push_back(v>>(8*i)); }
std::uint16_t U16(const std::uint8_t* p) { return p[0] | std::uint16_t(p[1])<<8; }
std::uint32_t U32(const std::uint8_t* p) { std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(p[i])<<(8*i);return v; }
std::uint64_t U64(const std::uint8_t* p) { std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);return v; }
template<class A> bool Nz(const A& a) { return std::any_of(a.begin(),a.end(),[](auto b){return b!=0;}); }
template<class A> void Put(std::vector<std::uint8_t>* o,const A& a){o->insert(o->end(),a.begin(),a.end());}
template<class A> void Get(const std::uint8_t* p,A* a){std::copy_n(p,a->size(),a->begin());}
bool Magic(const std::uint8_t* b,std::string_view s){return std::equal(b,b+s.size(),reinterpret_cast<const std::uint8_t*>(s.data()));}
SblrVariableSha256V1 Hash(std::string_view domain,const std::vector<std::uint8_t>& bytes){
  std::vector<std::uint8_t> in(domain.begin(),domain.end());in.insert(in.end(),bytes.begin(),bytes.end());
  const auto d=core::hash::ComputeSha256Digest(in);return d.ok()?d.digest:SblrVariableSha256V1{};
}
constexpr std::string_view kExecutor="engine.op.variable",kResult="typed_value";
bool ValidState(std::uint8_t s){return s>=1&&s<=3;}
}

std::vector<std::uint8_t> EncodeSblrVariableNodeTableV1(const SblrVariableNodeTableV1& t){
  if(t.nodes.empty()||t.nodes.size()>4096)return{};std::vector<std::uint8_t> o;o.reserve(32+t.nodes.size()*160);
  o.insert(o.end(),{'S','B','V','N'});P16(&o,1);P16(&o,32);P32(&o,t.nodes.size());P32(&o,0);P64(&o,32+t.nodes.size()*160);P64(&o,32);
  std::uint64_t prior_node=0;std::uint32_t prior_ordinal=0;
  for(const auto& n:t.nodes){if(n.node_id<=prior_node||n.parent_operand_ordinal!=prior_ordinal+1||!Nz(n.scope_uuid)||!n.scope_generation||!Nz(n.frame_uuid)||!n.frame_generation||!Nz(n.variable_descriptor_uuid)||!n.variable_descriptor_generation||!Nz(n.datatype_descriptor_uuid)||!n.datatype_descriptor_generation||!n.value_generation||!ValidState(n.value_state_policy))return{};
    P32(&o,160);P64(&o,n.node_id);P32(&o,n.parent_operand_ordinal);Put(&o,n.scope_uuid);P64(&o,n.scope_generation);Put(&o,n.frame_uuid);P64(&o,n.frame_generation);Put(&o,n.variable_descriptor_uuid);P64(&o,n.variable_descriptor_generation);Put(&o,n.datatype_descriptor_uuid);P64(&o,n.datatype_descriptor_generation);P64(&o,n.value_generation);P16(&o,kExecutor.size());o.insert(o.end(),kExecutor.begin(),kExecutor.end());P16(&o,kResult.size());o.insert(o.end(),kResult.begin(),kResult.end());P16(&o,1);o.push_back(n.value_state_policy);P32(&o,0);prior_node=n.node_id;prior_ordinal=n.parent_operand_ordinal;}
  return o;
}
SblrVariableNodeTableCodecResultV1 DecodeSblrVariableNodeTableV1(const std::uint8_t* b,std::size_t z){
  SblrVariableNodeTableCodecResultV1 r;auto fail=[&](std::string d){r.detail=std::move(d);return r;};
  if(!b||z<32||!Magic(b,"SBVN")||U16(b+4)!=1||U16(b+6)!=32||U32(b+12)||U64(b+16)!=z||U64(b+24)!=32)return fail("SBVN header is structurally invalid");
  const auto count=U32(b+8);if(!count)return fail("empty SBVN is forbidden");if(count>(std::numeric_limits<std::size_t>::max()-32)/160||32+std::size_t(count)*160!=z)return fail("SBVN extent is structurally invalid");
  std::uint64_t prior_node=0;std::uint32_t prior_ordinal=0;
  for(std::uint32_t i=0;i<count;++i){const auto x=32+std::size_t(i)*160;SblrVariableNodeV1 n;n.node_id=U64(b+x+4);n.parent_operand_ordinal=U32(b+x+12);Get(b+x+16,&n.scope_uuid);n.scope_generation=U64(b+x+32);Get(b+x+40,&n.frame_uuid);n.frame_generation=U64(b+x+56);Get(b+x+64,&n.variable_descriptor_uuid);n.variable_descriptor_generation=U64(b+x+80);Get(b+x+88,&n.datatype_descriptor_uuid);n.datatype_descriptor_generation=U64(b+x+104);n.value_generation=U64(b+x+112);n.value_state_policy=b[x+155];
    if(U32(b+x)!=160||n.node_id<=prior_node||n.parent_operand_ordinal!=prior_ordinal+1||!Nz(n.scope_uuid)||!n.scope_generation||!Nz(n.frame_uuid)||!n.frame_generation||!Nz(n.variable_descriptor_uuid)||!n.variable_descriptor_generation||!Nz(n.datatype_descriptor_uuid)||!n.datatype_descriptor_generation||!n.value_generation||U16(b+x+120)!=kExecutor.size()||!std::equal(b+x+122,b+x+140,kExecutor.begin())||U16(b+x+140)!=kResult.size()||!std::equal(b+x+142,b+x+153,kResult.begin())||U16(b+x+153)!=1||!ValidState(n.value_state_policy)||U32(b+x+156))return fail("SBVN record is structurally noncanonical");
    prior_node=n.node_id;prior_ordinal=n.parent_operand_ordinal;r.table.nodes.push_back(n);}
  if(count>4096||z>655392){r.diagnostic_id="RESOURCE.BUDGET_EXCEEDED";return fail("SBVN exceeds admitted node budget");}
  r.canonical_bytes=EncodeSblrVariableNodeTableV1(r.table);if(r.canonical_bytes.size()!=z||!std::equal(r.canonical_bytes.begin(),r.canonical_bytes.end(),b))return fail("SBVN decode reencode differs");r.ok=true;return r;
}
std::vector<std::uint8_t> EncodeSblrVariableNodeReferenceV1(const SblrVariableNodeReferenceV1& v){
  if(!v.occurrence_ordinal||!v.node_id||!Nz(v.table_sha256)||!Nz(v.scope_uuid)||!v.scope_generation||!Nz(v.frame_uuid)||!v.frame_generation||!Nz(v.variable_descriptor_uuid)||!v.variable_descriptor_generation||!v.value_generation)return{};
  std::vector<std::uint8_t> o;P16(&o,1);P16(&o,0);P32(&o,v.occurrence_ordinal);P64(&o,v.node_id);Put(&o,v.table_sha256);Put(&o,v.scope_uuid);P64(&o,v.scope_generation);Put(&o,v.frame_uuid);P64(&o,v.frame_generation);Put(&o,v.variable_descriptor_uuid);P64(&o,v.variable_descriptor_generation);P64(&o,v.value_generation);P64(&o,0);return o;
}
bool DecodeSblrVariableNodeReferenceV1(const std::uint8_t* b,std::size_t z,SblrVariableNodeReferenceV1* out){if(!b||!out||z!=136||U16(b)!=1||U16(b+2)||U64(b+128))return false;SblrVariableNodeReferenceV1 v;v.occurrence_ordinal=U32(b+4);v.node_id=U64(b+8);Get(b+16,&v.table_sha256);Get(b+48,&v.scope_uuid);v.scope_generation=U64(b+64);Get(b+72,&v.frame_uuid);v.frame_generation=U64(b+88);Get(b+96,&v.variable_descriptor_uuid);v.variable_descriptor_generation=U64(b+112);v.value_generation=U64(b+120);if(EncodeSblrVariableNodeReferenceV1(v).size()!=136)return false;*out=v;return true;}
bool ValidateSblrVariableReferenceBijectionV1(const SblrVariableNodeTableCodecResultV1& t,const std::vector<SblrVariableNodeReferenceV1>& refs){if(!t.ok||refs.size()!=t.table.nodes.size())return false;const auto digest=Hash("ScratchBird.SblrVariableNodeTable.V1",t.canonical_bytes);if(!Nz(digest))return false;std::vector<bool> seen(refs.size());for(const auto& r:refs){auto it=std::find_if(t.table.nodes.begin(),t.table.nodes.end(),[&](const auto& n){return n.node_id==r.node_id;});if(it==t.table.nodes.end())return false;auto i=std::distance(t.table.nodes.begin(),it);if(seen[i]||r.table_sha256!=digest||r.occurrence_ordinal!=it->parent_operand_ordinal||r.scope_uuid!=it->scope_uuid||r.scope_generation!=it->scope_generation||r.frame_uuid!=it->frame_uuid||r.frame_generation!=it->frame_generation||r.variable_descriptor_uuid!=it->variable_descriptor_uuid||r.variable_descriptor_generation!=it->variable_descriptor_generation||r.value_generation!=it->value_generation)return false;seen[i]=true;}return true;}

SblrVariableSha256V1 ComputeSblrVariableDemandSha256V1(const std::vector<SblrVariableDemandV1>& rows){std::vector<std::uint8_t> b;for(const auto&r:rows){P64(&b,r.occurrence_id);P32(&b,r.parent_operand_ordinal);P32(&b,r.variable_ordinal);Put(&b,r.scope_uuid);}return Hash("ScratchBird.SblrVariableDemandSequence.V1",b);}
SblrVariableSha256V1 ComputeSblrVariableMappingSha256V1(const std::vector<SblrVariableMappingV1>& rows){std::vector<std::uint8_t>b;for(const auto&r:rows){P64(&b,r.occurrence_id);P32(&b,r.variable_ordinal);b.push_back(r.nullable);b.push_back(r.mutability);P16(&b,0);Put(&b,r.variable_descriptor_uuid);P64(&b,r.variable_descriptor_generation);Put(&b,r.datatype_descriptor_uuid);P64(&b,r.datatype_descriptor_generation);Put(&b,r.datatype_type_uuid);P64(&b,r.value_generation);b.push_back(r.value_state);b.insert(b.end(),7,0);}return Hash("ScratchBird.SblrVariableDescriptorMappings.V1",b);}

SblrVariableSha256V1 ComputeSblrVariableFrameDemandSha256V1(const std::vector<SblrVariableFrameDemandV1>&rows){std::vector<std::uint8_t>b;for(const auto&r:rows){P64(&b,r.declaration_occurrence_id);P16(&b,r.datatype_context_code);b.push_back(r.nullable);b.push_back(r.mutability);b.push_back(r.initial_value_state);b.insert(b.end(),3,0);Put(&b,r.declaration_token_sha256);}return Hash("ScratchBird.SblrVariableFrameDemands.V1",b);}
SblrVariableSha256V1 ComputeSblrVariableFrameMappingSha256V1(const std::vector<SblrVariableFrameMappingV1>&rows){std::vector<std::uint8_t>b;for(const auto&r:rows){P64(&b,r.declaration_occurrence_id);P32(&b,r.variable_ordinal);b.push_back(r.nullable);b.push_back(r.mutability);b.push_back(r.value_state);b.push_back(0);Put(&b,r.variable_descriptor_uuid);P64(&b,r.variable_descriptor_generation);Put(&b,r.datatype_descriptor_uuid);P64(&b,r.datatype_descriptor_generation);Put(&b,r.datatype_type_uuid);P64(&b,r.value_generation);}return Hash("ScratchBird.SblrVariableFrameMappings.V1",b);}
std::vector<std::uint8_t> EncodeSblrVariableFrameBeginRequestV1(SblrVariableFrameBeginRequestV1*v){if(!v||!Nz(v->operation_uuid)||!Nz(v->transaction_uuid)||v->demands.empty()||v->demands.size()>4096)return{};std::uint64_t prior=0;for(const auto&r:v->demands){if(!r.declaration_occurrence_id||r.declaration_occurrence_id<=prior||r.datatype_context_code!=1||r.nullable>1||r.mutability>1||(r.initial_value_state!=2&&r.initial_value_state!=3)|| (r.initial_value_state==2&&!r.nullable)||!Nz(r.declaration_token_sha256))return{};prior=r.declaration_occurrence_id;}v->demand_sha256=ComputeSblrVariableFrameDemandSha256V1(v->demands);std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','B'});P16(&o,1);P16(&o,96);P32(&o,96+v->demands.size()*48);P32(&o,0);Put(&o,v->operation_uuid);Put(&o,v->transaction_uuid);P32(&o,v->demands.size());P32(&o,48);Put(&o,v->demand_sha256);P64(&o,v->expires_after_ns);for(const auto&r:v->demands){P64(&o,r.declaration_occurrence_id);P16(&o,r.datatype_context_code);o.push_back(r.nullable);o.push_back(r.mutability);o.push_back(r.initial_value_state);o.insert(o.end(),3,0);Put(&o,r.declaration_token_sha256);}return o;}
bool DecodeSblrVariableFrameBeginRequestV1(const std::uint8_t*b,std::size_t z,SblrVariableFrameBeginRequestV1*out,std::string*e){if(!b||!out||z<96||!Magic(b,"SBVB")||U16(b+4)!=1||U16(b+6)!=96||U32(b+8)!=z||U32(b+12)||U32(b+52)!=48){if(e)*e="SBVB header invalid";return false;}auto n=U32(b+48);if(!n||n>4096||96+std::size_t(n)*48!=z){if(e)*e="SBVB extent invalid";return false;}SblrVariableFrameBeginRequestV1 v;Get(b+16,&v.operation_uuid);Get(b+32,&v.transaction_uuid);Get(b+56,&v.demand_sha256);v.expires_after_ns=U64(b+88);for(std::uint32_t i=0;i<n;++i){auto x=96+i*48;SblrVariableFrameDemandV1 r;r.declaration_occurrence_id=U64(b+x);r.datatype_context_code=U16(b+x+8);r.nullable=b[x+10];r.mutability=b[x+11];r.initial_value_state=b[x+12];if(std::any_of(b+x+13,b+x+16,[](auto q){return q!=0;})){if(e)*e="SBVB reserved invalid";return false;}Get(b+x+16,&r.declaration_token_sha256);v.demands.push_back(r);}auto copy=v;auto enc=EncodeSblrVariableFrameBeginRequestV1(&copy);if(enc.size()!=z||!std::equal(enc.begin(),enc.end(),b)){if(e)*e="SBVB noncanonical";return false;}*out=std::move(v);return true;}
std::vector<std::uint8_t> EncodeSblrVariableFrameBeginResultV1(SblrVariableFrameBeginResultV1*v){if(!v||!Nz(v->public_coordination_uuid)||!Nz(v->operation_uuid)||!Nz(v->scope_uuid)||!v->scope_generation||!Nz(v->frame_uuid)||!v->frame_generation||!v->registry_generation||!v->coordinator_generation||v->mappings.empty()||v->mappings.size()>4096)return{};std::uint64_t prior=0;std::uint32_t ordinal=0;for(const auto&r:v->mappings){if(!r.declaration_occurrence_id||r.declaration_occurrence_id<=prior||r.variable_ordinal!=ordinal++||r.nullable>1||r.mutability>1||(r.value_state!=2&&r.value_state!=3)||!Nz(r.variable_descriptor_uuid)||!r.variable_descriptor_generation||!Nz(r.datatype_descriptor_uuid)||!r.datatype_descriptor_generation||!Nz(r.datatype_type_uuid)||!r.value_generation)return{};prior=r.declaration_occurrence_id;}v->mapping_sha256=ComputeSblrVariableFrameMappingSha256V1(v->mappings);std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','C'});P16(&o,1);P16(&o,152);P32(&o,152+v->mappings.size()*88);P32(&o,0);Put(&o,v->public_coordination_uuid);Put(&o,v->operation_uuid);Put(&o,v->scope_uuid);P64(&o,v->scope_generation);Put(&o,v->frame_uuid);P64(&o,v->frame_generation);P32(&o,v->mappings.size());P32(&o,88);Put(&o,v->mapping_sha256);P64(&o,v->registry_generation);P64(&o,v->coordinator_generation);for(const auto&r:v->mappings){P64(&o,r.declaration_occurrence_id);P32(&o,r.variable_ordinal);o.push_back(r.nullable);o.push_back(r.mutability);o.push_back(r.value_state);o.push_back(0);Put(&o,r.variable_descriptor_uuid);P64(&o,r.variable_descriptor_generation);Put(&o,r.datatype_descriptor_uuid);P64(&o,r.datatype_descriptor_generation);Put(&o,r.datatype_type_uuid);P64(&o,r.value_generation);}return o;}
bool DecodeSblrVariableFrameBeginResultV1(const std::uint8_t*b,std::size_t z,SblrVariableFrameBeginResultV1*out,std::string*e){if(!b||!out||z<152||!Magic(b,"SBVC")||U16(b+4)!=1||U16(b+6)!=152||U32(b+8)!=z||U32(b+12)||U32(b+100)!=88){if(e)*e="SBVC header invalid";return false;}auto n=U32(b+96);if(!n||n>4096||152+std::size_t(n)*88!=z){if(e)*e="SBVC extent invalid";return false;}SblrVariableFrameBeginResultV1 v;Get(b+16,&v.public_coordination_uuid);Get(b+32,&v.operation_uuid);Get(b+48,&v.scope_uuid);v.scope_generation=U64(b+64);Get(b+72,&v.frame_uuid);v.frame_generation=U64(b+88);Get(b+104,&v.mapping_sha256);v.registry_generation=U64(b+136);v.coordinator_generation=U64(b+144);for(std::uint32_t i=0;i<n;++i){auto x=152+i*88;SblrVariableFrameMappingV1 r;r.declaration_occurrence_id=U64(b+x);r.variable_ordinal=U32(b+x+8);r.nullable=b[x+12];r.mutability=b[x+13];r.value_state=b[x+14];if(b[x+15]){if(e)*e="SBVC reserved invalid";return false;}Get(b+x+16,&r.variable_descriptor_uuid);r.variable_descriptor_generation=U64(b+x+32);Get(b+x+40,&r.datatype_descriptor_uuid);r.datatype_descriptor_generation=U64(b+x+56);Get(b+x+64,&r.datatype_type_uuid);r.value_generation=U64(b+x+80);v.mappings.push_back(r);}auto copy=v;auto enc=EncodeSblrVariableFrameBeginResultV1(&copy);if(enc.size()!=z||!std::equal(enc.begin(),enc.end(),b)){if(e)*e="SBVC noncanonical";return false;}*out=std::move(v);return true;}
std::vector<std::uint8_t> EncodeSblrVariableFrameCloseRequestV1(const SblrVariableFrameCloseRequestV1&v){if(!Nz(v.public_coordination_uuid)||!Nz(v.operation_uuid)||!v.expected_frame_generation)return{};std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','X'});P16(&o,1);P16(&o,64);P32(&o,64);P32(&o,0);Put(&o,v.public_coordination_uuid);Put(&o,v.operation_uuid);P64(&o,v.expected_frame_generation);P32(&o,v.reason_code);P32(&o,0);return o;}
bool DecodeSblrVariableFrameCloseRequestV1(const std::uint8_t*b,std::size_t z,SblrVariableFrameCloseRequestV1*out,std::string*e){if(!b||!out||z!=64||!Magic(b,"SBVX")||U16(b+4)!=1||U16(b+6)!=64||U32(b+8)!=64||U32(b+12)||U32(b+60)){if(e)*e="SBVX invalid";return false;}SblrVariableFrameCloseRequestV1 v;Get(b+16,&v.public_coordination_uuid);Get(b+32,&v.operation_uuid);v.expected_frame_generation=U64(b+48);v.reason_code=U32(b+56);if(EncodeSblrVariableFrameCloseRequestV1(v).size()!=64)return false;*out=v;return true;}
std::vector<std::uint8_t> EncodeSblrVariableFrameCloseResultV1(const SblrVariableFrameCloseResultV1&v){if(!Nz(v.public_coordination_uuid)||!v.revoked_frame_generation||!v.decision_evidence_generation)return{};std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','Z'});P16(&o,1);P16(&o,48);P32(&o,48);P32(&o,0);Put(&o,v.public_coordination_uuid);P64(&o,v.revoked_frame_generation);P64(&o,v.decision_evidence_generation);return o;}
bool DecodeSblrVariableFrameCloseResultV1(const std::uint8_t*b,std::size_t z,SblrVariableFrameCloseResultV1*out,std::string*e){if(!b||!out||z!=48||!Magic(b,"SBVZ")||U16(b+4)!=1||U16(b+6)!=48||U32(b+8)!=48||U32(b+12)){if(e)*e="SBVZ invalid";return false;}SblrVariableFrameCloseResultV1 v;Get(b+16,&v.public_coordination_uuid);v.revoked_frame_generation=U64(b+32);v.decision_evidence_generation=U64(b+40);if(EncodeSblrVariableFrameCloseResultV1(v).size()!=48)return false;*out=v;return true;}

std::vector<std::uint8_t> EncodeSblrVariableNegotiateRequestV1(SblrVariableNegotiateRequestV1* v){if(!v||v->demands.empty()||v->demands.size()>4096||!Nz(v->preliminary_receipt_uuid)||!Nz(v->scope_uuid)||!v->scope_generation||!Nz(v->frame_uuid)||!v->frame_generation)return{};std::uint32_t ord=0;for(const auto&r:v->demands)if(!r.occurrence_id||r.parent_operand_ordinal!=++ord||r.scope_uuid!=v->scope_uuid)return{};v->demand_sha256=ComputeSblrVariableDemandSha256V1(v->demands);std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','R'});P16(&o,1);P16(&o,128);P32(&o,128+v->demands.size()*32);P32(&o,0);Put(&o,v->preliminary_receipt_uuid);Put(&o,v->scope_uuid);P64(&o,v->scope_generation);Put(&o,v->frame_uuid);P64(&o,v->frame_generation);P32(&o,v->demands.size());P32(&o,32);Put(&o,v->demand_sha256);P64(&o,0);for(const auto&r:v->demands){P64(&o,r.occurrence_id);P32(&o,r.parent_operand_ordinal);P32(&o,r.variable_ordinal);Put(&o,r.scope_uuid);}return o;}
bool DecodeSblrVariableNegotiateRequestV1(const std::uint8_t*b,std::size_t z,SblrVariableNegotiateRequestV1*out,std::string*e){if(!b||!out||z<128||!Magic(b,"SBVR")||U16(b+4)!=1||U16(b+6)!=128||U32(b+8)!=z||U32(b+12)||U32(b+84)!=32||U64(b+120)){if(e)*e="SBVR header invalid";return false;}auto n=U32(b+80);if(!n||n>4096||128+std::size_t(n)*32!=z){if(e)*e="SBVR extent invalid";return false;}SblrVariableNegotiateRequestV1 v;Get(b+16,&v.preliminary_receipt_uuid);Get(b+32,&v.scope_uuid);v.scope_generation=U64(b+48);Get(b+56,&v.frame_uuid);v.frame_generation=U64(b+72);Get(b+88,&v.demand_sha256);for(std::uint32_t i=0;i<n;++i){auto x=128+i*32;SblrVariableDemandV1 r;r.occurrence_id=U64(b+x);r.parent_operand_ordinal=U32(b+x+8);r.variable_ordinal=U32(b+x+12);Get(b+x+16,&r.scope_uuid);v.demands.push_back(r);}auto copy=v;auto enc=EncodeSblrVariableNegotiateRequestV1(&copy);if(enc.size()!=z||!std::equal(enc.begin(),enc.end(),b)){if(e)*e="SBVR noncanonical";return false;}*out=std::move(v);return true;}

std::vector<std::uint8_t> EncodeSblrVariableNegotiateResultV1(SblrVariableNegotiateResultV1* v){if(!v||v->mappings.empty()||v->mappings.size()>4096||!Nz(v->preliminary_receipt_uuid)||!Nz(v->scope_uuid)||!v->scope_generation||!Nz(v->frame_uuid)||!v->frame_generation||!Nz(v->registry_snapshot_uuid)||!v->registry_generation)return{};std::uint64_t prior=0;for(const auto&r:v->mappings){if(!r.occurrence_id||r.occurrence_id<=prior||!Nz(r.variable_descriptor_uuid)||!r.variable_descriptor_generation||!Nz(r.datatype_descriptor_uuid)||!r.datatype_descriptor_generation||!Nz(r.datatype_type_uuid)||!r.value_generation||r.nullable>1||r.mutability>1||!ValidState(r.value_state))return{};prior=r.occurrence_id;}v->mapping_sha256=ComputeSblrVariableMappingSha256V1(v->mappings);std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','G'});P16(&o,1);P16(&o,144);P32(&o,144+v->mappings.size()*96);P32(&o,0);Put(&o,v->preliminary_receipt_uuid);Put(&o,v->scope_uuid);P64(&o,v->scope_generation);Put(&o,v->frame_uuid);P64(&o,v->frame_generation);P32(&o,v->mappings.size());P32(&o,96);Put(&o,v->mapping_sha256);Put(&o,v->registry_snapshot_uuid);P64(&o,v->registry_generation);for(const auto&r:v->mappings){P64(&o,r.occurrence_id);P32(&o,r.variable_ordinal);o.push_back(r.nullable);o.push_back(r.mutability);P16(&o,0);Put(&o,r.variable_descriptor_uuid);P64(&o,r.variable_descriptor_generation);Put(&o,r.datatype_descriptor_uuid);P64(&o,r.datatype_descriptor_generation);Put(&o,r.datatype_type_uuid);P64(&o,r.value_generation);o.push_back(r.value_state);o.insert(o.end(),7,0);}return o;}
bool DecodeSblrVariableNegotiateResultV1(const std::uint8_t*b,std::size_t z,SblrVariableNegotiateResultV1*out,std::string*e){if(!b||!out||z<144||!Magic(b,"SBVG")||U16(b+4)!=1||U16(b+6)!=144||U32(b+8)!=z||U32(b+12)||U32(b+84)!=96){if(e)*e="SBVG header invalid";return false;}auto n=U32(b+80);if(!n||n>4096||144+std::size_t(n)*96!=z){if(e)*e="SBVG extent invalid";return false;}SblrVariableNegotiateResultV1 v;Get(b+16,&v.preliminary_receipt_uuid);Get(b+32,&v.scope_uuid);v.scope_generation=U64(b+48);Get(b+56,&v.frame_uuid);v.frame_generation=U64(b+72);Get(b+88,&v.mapping_sha256);Get(b+120,&v.registry_snapshot_uuid);v.registry_generation=U64(b+136);for(std::uint32_t i=0;i<n;++i){auto x=144+i*96;SblrVariableMappingV1 r;r.occurrence_id=U64(b+x);r.variable_ordinal=U32(b+x+8);r.nullable=b[x+12];r.mutability=b[x+13];if(U16(b+x+14)||std::any_of(b+x+89,b+x+96,[](auto q){return q!=0;})){if(e)*e="SBVG reserved invalid";return false;}Get(b+x+16,&r.variable_descriptor_uuid);r.variable_descriptor_generation=U64(b+x+32);Get(b+x+40,&r.datatype_descriptor_uuid);r.datatype_descriptor_generation=U64(b+x+56);Get(b+x+64,&r.datatype_type_uuid);r.value_generation=U64(b+x+80);r.value_state=b[x+88];v.mappings.push_back(r);}auto copy=v;auto enc=EncodeSblrVariableNegotiateResultV1(&copy);if(enc.size()!=z||!std::equal(enc.begin(),enc.end(),b)){if(e)*e="SBVG noncanonical";return false;}*out=std::move(v);return true;}

std::vector<std::uint8_t> EncodeSblrVariableFinalizeRequestV1(const SblrVariableFinalizeRequestV1&v){if(!Nz(v.preliminary_receipt_uuid)||!Nz(v.scope_uuid)||!v.scope_generation||!Nz(v.frame_uuid)||!v.frame_generation||!v.registry_generation||v.canonical_sbvn.empty()||v.canonical_sbvn.size()>655392)return{};std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','F'});P16(&o,1);P16(&o,192);P32(&o,192+v.canonical_sbvn.size());P32(&o,0);Put(&o,v.preliminary_receipt_uuid);Put(&o,v.scope_uuid);P64(&o,v.scope_generation);Put(&o,v.frame_uuid);P64(&o,v.frame_generation);Put(&o,v.demand_sha256);Put(&o,v.mapping_sha256);Put(&o,v.sbvn_sha256);P32(&o,v.canonical_sbvn.size());P64(&o,v.registry_generation);P32(&o,0);o.insert(o.end(),v.canonical_sbvn.begin(),v.canonical_sbvn.end());return o;}
bool DecodeSblrVariableFinalizeRequestV1(const std::uint8_t*b,std::size_t z,SblrVariableFinalizeRequestV1*out,std::string*e){if(!b||!out||z<192||!Magic(b,"SBVF")||U16(b+4)!=1||U16(b+6)!=192||U32(b+8)!=z||U32(b+12)||U32(b+188)||U32(b+176)!=z-192){if(e)*e="SBVF header invalid";return false;}SblrVariableFinalizeRequestV1 v;Get(b+16,&v.preliminary_receipt_uuid);Get(b+32,&v.scope_uuid);v.scope_generation=U64(b+48);Get(b+56,&v.frame_uuid);v.frame_generation=U64(b+72);Get(b+80,&v.demand_sha256);Get(b+112,&v.mapping_sha256);Get(b+144,&v.sbvn_sha256);v.registry_generation=U64(b+180);v.canonical_sbvn.assign(b+192,b+z);const auto table=DecodeSblrVariableNodeTableV1(v.canonical_sbvn.data(),v.canonical_sbvn.size());if(!table.ok||Hash("ScratchBird.SblrVariableNodeTable.V1",v.canonical_sbvn)!=v.sbvn_sha256){if(e)*e="SBVF SBVN invalid";return false;}if(EncodeSblrVariableFinalizeRequestV1(v).size()!=z){if(e)*e="SBVF noncanonical";return false;}*out=std::move(v);return true;}

std::vector<std::uint8_t> EncodeSblrVariableAdmissionV1(SblrVariableAdmissionV1*v){if(!v||!Nz(v->final_receipt_uuid)||!Nz(v->admission_token_uuid)||!Nz(v->scope_uuid)||!v->scope_generation||!Nz(v->frame_uuid)||!v->frame_generation||!Nz(v->registry_snapshot_uuid)||!v->registry_generation||!v->executor_availability_generation)return{};std::vector<std::uint8_t> material;Put(&material,v->final_receipt_uuid);Put(&material,v->admission_token_uuid);Put(&material,v->scope_uuid);P64(&material,v->scope_generation);Put(&material,v->frame_uuid);P64(&material,v->frame_generation);Put(&material,v->registry_snapshot_uuid);P64(&material,v->registry_generation);P64(&material,v->executor_availability_generation);P64(&material,v->expires_at_monotonic_ns);v->binding_sha256=Hash("ScratchBird.SblrVariableBinding.V1",material);std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','A'});P16(&o,1);P16(&o,176);P32(&o,176);P32(&o,0);Put(&o,v->final_receipt_uuid);Put(&o,v->admission_token_uuid);Put(&o,v->scope_uuid);P64(&o,v->scope_generation);Put(&o,v->frame_uuid);P64(&o,v->frame_generation);Put(&o,v->registry_snapshot_uuid);P64(&o,v->registry_generation);P64(&o,v->executor_availability_generation);Put(&o,v->binding_sha256);P64(&o,v->expires_at_monotonic_ns);P64(&o,0);return o;}
bool DecodeSblrVariableAdmissionV1(const std::uint8_t*b,std::size_t z,SblrVariableAdmissionV1*out,std::string*e){if(!b||!out||z!=176||!Magic(b,"SBVA")||U16(b+4)!=1||U16(b+6)!=176||U32(b+8)!=176||U32(b+12)||U64(b+168)){if(e)*e="SBVA header invalid";return false;}SblrVariableAdmissionV1 v;Get(b+16,&v.final_receipt_uuid);Get(b+32,&v.admission_token_uuid);Get(b+48,&v.scope_uuid);v.scope_generation=U64(b+64);Get(b+72,&v.frame_uuid);v.frame_generation=U64(b+88);Get(b+96,&v.registry_snapshot_uuid);v.registry_generation=U64(b+112);v.executor_availability_generation=U64(b+120);Get(b+128,&v.binding_sha256);v.expires_at_monotonic_ns=U64(b+160);auto copy=v;auto enc=EncodeSblrVariableAdmissionV1(&copy);if(enc.size()!=z||!std::equal(enc.begin(),enc.end(),b)){if(e)*e="SBVA noncanonical";return false;}*out=v;return true;}

std::vector<std::uint8_t> EncodeSblrVariableExecutionBindingV1(const SblrVariableExecutionBindingV1&v){if(!Nz(v.execution_uuid)||!Nz(v.statement_receipt_uuid)||!Nz(v.variable_final_receipt_uuid)||!Nz(v.admission_token_uuid)||!Nz(v.scope_uuid)||!v.scope_generation||!Nz(v.frame_uuid)||!v.frame_generation||!Nz(v.registry_snapshot_uuid)||!v.registry_generation||!v.executor_availability_generation||!Nz(v.binding_sha256))return{};std::vector<std::uint8_t>o;o.insert(o.end(),{'S','B','V','E'});P16(&o,1);P16(&o,192);P32(&o,192);P32(&o,0);Put(&o,v.execution_uuid);Put(&o,v.statement_receipt_uuid);Put(&o,v.variable_final_receipt_uuid);Put(&o,v.admission_token_uuid);Put(&o,v.scope_uuid);P64(&o,v.scope_generation);Put(&o,v.frame_uuid);P64(&o,v.frame_generation);Put(&o,v.registry_snapshot_uuid);P64(&o,v.registry_generation);P64(&o,v.executor_availability_generation);Put(&o,v.binding_sha256);return o;}
bool DecodeSblrVariableExecutionBindingV1(const std::uint8_t*b,std::size_t z,SblrVariableExecutionBindingV1*out,std::string*e){if(!b||!out||z!=192||!Magic(b,"SBVE")||U16(b+4)!=1||U16(b+6)!=192||U32(b+8)!=192||U32(b+12)){if(e)*e="SBVE header invalid";return false;}SblrVariableExecutionBindingV1 v;Get(b+16,&v.execution_uuid);Get(b+32,&v.statement_receipt_uuid);Get(b+48,&v.variable_final_receipt_uuid);Get(b+64,&v.admission_token_uuid);Get(b+80,&v.scope_uuid);v.scope_generation=U64(b+96);Get(b+104,&v.frame_uuid);v.frame_generation=U64(b+120);Get(b+128,&v.registry_snapshot_uuid);v.registry_generation=U64(b+144);v.executor_availability_generation=U64(b+152);Get(b+160,&v.binding_sha256);if(EncodeSblrVariableExecutionBindingV1(v).size()!=192){if(e)*e="SBVE noncanonical";return false;}*out=v;return true;}

std::vector<std::uint8_t> EncodeSblrVariableAssignmentRequestV1(
    SblrVariableAssignmentRequestV1* v) {
  if (!v || v->assignments.empty() || v->assignments.size() > 4096 ||
      !Nz(v->preliminary_receipt_uuid) || !Nz(v->public_coordination_uuid) ||
      !Nz(v->operation_uuid) || !Nz(v->scope_uuid) ||
      !v->scope_generation || !Nz(v->frame_uuid) || !v->frame_generation ||
      !Nz(v->registry_snapshot_uuid) || !v->registry_generation)
    return {};
  std::vector<std::uint8_t> records;
  std::uint64_t prior = 0;
  for (auto& r : v->assignments) {
    if (!r.assignment_occurrence_id || r.assignment_occurrence_id <= prior ||
        !Nz(r.variable_descriptor_uuid) || !r.variable_descriptor_generation ||
        !r.expected_value_generation || !Nz(r.datatype_descriptor_uuid) ||
        !r.datatype_descriptor_generation ||
        (r.value_state != 1 && r.value_state != 2) ||
        r.canonical_value_bytes.size() > 65536 ||
        (r.value_state == 2 && !r.canonical_value_bytes.empty()))
      return {};
    prior = r.assignment_occurrence_id;
    r.canonical_value_sha256 = Hash("", r.canonical_value_bytes);
    P32(&records, 120 + r.canonical_value_bytes.size());
    P64(&records, r.assignment_occurrence_id); P32(&records, r.variable_ordinal);
    Put(&records, r.variable_descriptor_uuid);
    P64(&records, r.variable_descriptor_generation);
    P64(&records, r.expected_value_generation);
    Put(&records, r.datatype_descriptor_uuid);
    P64(&records, r.datatype_descriptor_generation);
    records.push_back(r.value_state); records.insert(records.end(), 7, 0);
    P32(&records, r.canonical_value_bytes.size()); P32(&records, 0);
    Put(&records, r.canonical_value_sha256);
    records.insert(records.end(), r.canonical_value_bytes.begin(),
                   r.canonical_value_bytes.end());
  }
  if (176 + records.size() > 33'554'432) return {};
  v->assignment_sha256 = Hash("ScratchBird.SblrVariableAssignments.V1", records);
  std::vector<std::uint8_t> out{'S','B','V','Y'};
  P16(&out,1); P16(&out,176); P32(&out,176+records.size()); P32(&out,0);
  Put(&out,v->preliminary_receipt_uuid); Put(&out,v->public_coordination_uuid);
  Put(&out,v->operation_uuid); Put(&out,v->scope_uuid);
  P64(&out,v->scope_generation); Put(&out,v->frame_uuid);
  P64(&out,v->frame_generation); Put(&out,v->registry_snapshot_uuid);
  P64(&out,v->registry_generation); P32(&out,v->assignments.size());
  P32(&out,120); Put(&out,v->assignment_sha256);
  out.insert(out.end(),records.begin(),records.end()); return out;
}

bool DecodeSblrVariableAssignmentRequestV1(
    const std::uint8_t* b, std::size_t z, SblrVariableAssignmentRequestV1* out,
    std::string* e) {
  if (!b || !out || z < 176 || z > 33'554'432 || !Magic(b,"SBVY") ||
      U16(b+4)!=1 || U16(b+6)!=176 || U32(b+8)!=z || U32(b+12) ||
      U32(b+140)!=120) {
    if(e)*e="SBVY header invalid"; return false;
  }
  const auto count=U32(b+136); if(!count||count>4096){if(e)*e="SBVY count invalid";return false;}
  SblrVariableAssignmentRequestV1 v; Get(b+16,&v.preliminary_receipt_uuid);
  Get(b+32,&v.public_coordination_uuid); Get(b+48,&v.operation_uuid);
  Get(b+64,&v.scope_uuid); v.scope_generation=U64(b+80); Get(b+88,&v.frame_uuid);
  v.frame_generation=U64(b+104); Get(b+112,&v.registry_snapshot_uuid);
  v.registry_generation=U64(b+128); Get(b+144,&v.assignment_sha256);
  std::size_t p=176; for(std::uint32_t i=0;i<count;++i){
    if(z-p<120){if(e)*e="SBVY record truncated";return false;} const auto n=U32(b+p);
    if(n<120||n>120+65536||n>z-p||U32(b+p+84)||
       std::any_of(b+p+73,b+p+80,[](auto x){return x!=0;})){if(e)*e="SBVY record invalid";return false;}
    SblrVariableAssignmentRecordV1 r; r.assignment_occurrence_id=U64(b+p+4);
    r.variable_ordinal=U32(b+p+12); Get(b+p+16,&r.variable_descriptor_uuid);
    r.variable_descriptor_generation=U64(b+p+32); r.expected_value_generation=U64(b+p+40);
    Get(b+p+48,&r.datatype_descriptor_uuid); r.datatype_descriptor_generation=U64(b+p+64);
    r.value_state=b[p+72]; const auto bytes=U32(b+p+80); if(bytes!=n-120){if(e)*e="SBVY value extent invalid";return false;}
    Get(b+p+88,&r.canonical_value_sha256); r.canonical_value_bytes.assign(b+p+120,b+p+n);
    v.assignments.push_back(std::move(r)); p+=n;
  }
  if(p!=z){if(e)*e="SBVY trailing bytes";return false;} auto copy=v;
  const auto encoded=EncodeSblrVariableAssignmentRequestV1(&copy);
  if(encoded.size()!=z||!std::equal(encoded.begin(),encoded.end(),b)){if(e)*e="SBVY noncanonical";return false;}
  *out=std::move(v); return true;
}

std::vector<std::uint8_t> EncodeSblrVariableAssignmentResultV1(
    SblrVariableAssignmentResultV1* v) {
  if(!v||v->results.empty()||v->results.size()>4096||
     !Nz(v->preliminary_receipt_uuid)||!Nz(v->public_coordination_uuid)||
     !Nz(v->scope_uuid)||!v->scope_generation||!Nz(v->frame_uuid)||!v->frame_generation)return{};
  std::vector<std::uint8_t> records; std::uint64_t prior=0;
  for(const auto&r:v->results){if(!r.assignment_occurrence_id||r.assignment_occurrence_id<=prior||!Nz(r.variable_descriptor_uuid)||!r.variable_descriptor_generation||!r.new_value_generation||!r.decision_evidence_generation)return{};prior=r.assignment_occurrence_id;P64(&records,r.assignment_occurrence_id);P32(&records,r.variable_ordinal);P32(&records,0);Put(&records,r.variable_descriptor_uuid);P64(&records,r.variable_descriptor_generation);P64(&records,r.new_value_generation);P64(&records,r.decision_evidence_generation);}
  v->result_sha256=Hash("ScratchBird.SblrVariableAssignmentResults.V1",records);
  std::vector<std::uint8_t> o{'S','B','V','W'};P16(&o,1);P16(&o,152);P32(&o,152+records.size());P32(&o,0);Put(&o,v->preliminary_receipt_uuid);Put(&o,v->public_coordination_uuid);Put(&o,v->scope_uuid);P64(&o,v->scope_generation);Put(&o,v->frame_uuid);P64(&o,v->frame_generation);P64(&o,v->new_registry_generation);P32(&o,v->results.size());P32(&o,56);P64(&o,0);Put(&o,v->result_sha256);o.insert(o.end(),records.begin(),records.end());return o;
}

bool DecodeSblrVariableAssignmentResultV1(const std::uint8_t*b,std::size_t z,SblrVariableAssignmentResultV1*out,std::string*e){if(!b||!out||z<152||!Magic(b,"SBVW")||U16(b+4)!=1||U16(b+6)!=152||U32(b+8)!=z||U32(b+12)||U32(b+108)!=56||U64(b+112)){if(e)*e="SBVW header invalid";return false;}const auto n=U32(b+104);if(!n||n>4096||152+std::size_t(n)*56!=z){if(e)*e="SBVW extent invalid";return false;}SblrVariableAssignmentResultV1 v;Get(b+16,&v.preliminary_receipt_uuid);Get(b+32,&v.public_coordination_uuid);Get(b+48,&v.scope_uuid);v.scope_generation=U64(b+64);Get(b+72,&v.frame_uuid);v.frame_generation=U64(b+88);v.new_registry_generation=U64(b+96);Get(b+120,&v.result_sha256);for(std::uint32_t i=0;i<n;++i){auto p=152+i*56;if(U32(b+p+12)){if(e)*e="SBVW reserved invalid";return false;}SblrVariableAssignmentResultRecordV1 r;r.assignment_occurrence_id=U64(b+p);r.variable_ordinal=U32(b+p+8);Get(b+p+16,&r.variable_descriptor_uuid);r.variable_descriptor_generation=U64(b+p+32);r.new_value_generation=U64(b+p+40);r.decision_evidence_generation=U64(b+p+48);v.results.push_back(r);}auto copy=v;auto encoded=EncodeSblrVariableAssignmentResultV1(&copy);if(encoded.size()!=z||!std::equal(encoded.begin(),encoded.end(),b)){if(e)*e="SBVW noncanonical";return false;}*out=std::move(v);return true;}
}  // namespace scratchbird::engine::sblr
