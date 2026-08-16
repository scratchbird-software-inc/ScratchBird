// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_literal_runtime.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace sblr=scratchbird::engine::sblr;
namespace hash=scratchbird::core::hash;
using Bytes=std::vector<std::uint8_t>;
constexpr std::uint64_t kSeed=0x53424c495446555aULL; // SBLITFUZ
constexpr std::size_t kIterations=512;
[[noreturn]] void Fail(const std::string& s){std::cerr<<"seed=0x"<<std::hex<<kSeed<<std::dec<<' '<<s<<'\n';std::exit(EXIT_FAILURE);}
void Require(bool v,const std::string& s){if(!v)Fail(s);}
void U16(Bytes* o,std::uint16_t v){o->push_back(v);o->push_back(v>>8);}
void U32(Bytes* o,std::uint32_t v){for(unsigned i=0;i<4;++i)o->push_back(v>>(8*i));}
void U64(Bytes* o,std::uint64_t v){for(unsigned i=0;i<8;++i)o->push_back(v>>(8*i));}
std::uint16_t R16(const std::uint8_t*p){return p[0]|std::uint16_t(p[1])<<8;}
std::uint32_t R32(const std::uint8_t*p){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(p[i])<<(8*i);return v;}
std::uint64_t Next(std::uint64_t*s){auto x=*s;x^=x<<13;x^=x>>7;x^=x<<17;return *s=x;}
template<class F> void Fuzz(const char* name,const Bytes& seed,F&& f){
  Require(!seed.empty(),std::string(name)+" seed absent");auto start=std::chrono::steady_clock::now();
  std::uint64_t state=kSeed;const std::array<std::size_t,8> corpus{0,4,6,8,12,seed.size()/2,seed.size()-1,seed.size()};
  for(auto at:corpus){auto b=seed;if(at==b.size())b.push_back(0);else b[at]^=std::uint8_t(1u<<(at%8));f(b);}
  for(std::size_t i=0;i<kIterations;++i){Require(std::chrono::steady_clock::now()-start<std::chrono::seconds(5),std::string(name)+" timeout");auto b=seed;switch(Next(&state)%3){case 0:b[Next(&state)%b.size()]^=std::uint8_t(1u<<(Next(&state)%8));break;case 1:b.resize(Next(&state)%b.size());break;default:b.push_back(std::uint8_t(Next(&state)));}f(b);}
}
bool Sblq(const Bytes& b){
  if(b.size()<160||!std::equal(b.begin(),b.begin()+4,"SBLQ")||R16(b.data()+4)!=1||R16(b.data()+6)!=160||R32(b.data()+8)!=b.size()||R32(b.data()+12)||R32(b.data()+124)!=b.size()-160)return false;
  sblr::SblrLiteralPrebindResultV1 q;std::copy_n(b.begin()+16,16,q.preliminary_receipt_uuid.begin());std::copy_n(b.begin()+32,16,q.catalog_snapshot_uuid.begin());q.catalog_generation=R32(b.data()+48);std::copy_n(b.begin()+72,16,q.mga_snapshot_uuid.begin());std::copy_n(b.begin()+88,32,q.demand_sha256.begin());std::copy_n(b.begin()+128,32,q.ordered_profile_sha256.begin());
  std::size_t o=160;std::uint64_t prior=0;for(std::uint32_t i=0;i<R32(b.data()+120);++i){if(o>b.size()||b.size()-o<12)return false;auto id=R32(b.data()+o),n=R32(b.data()+o+8);if(id<=prior||n>b.size()-o-12)return false;sblr::SblrLiteralProfileMappingV1 m;m.occurrence_id=id;m.sblp_bytes.assign(b.begin()+o+12,b.begin()+o+12+n);if(!sblr::DecodeSblrLiteralDescriptorProfileV1(m.sblp_bytes.data(),m.sblp_bytes.size()).ok)return false;q.mappings.push_back(std::move(m));prior=id;o+=12+n;}
  return o==b.size()&&sblr::ComputeSblrLiteralOrderedProfilesSha256V1(q.mappings)==q.ordered_profile_sha256&&sblr::EncodeSblrLiteralPrebindResultV1(q)==b;
}
bool Sbla(const Bytes& b){
  if(b.size()!=264||!std::equal(b.begin(),b.begin()+4,"SBLA")||R16(b.data()+4)!=1||R16(b.data()+6)!=264||R32(b.data()+8)!=264||R32(b.data()+12))return false;sblr::SblrLiteralAdmissionV1 a;
  std::copy_n(b.begin()+16,16,a.preliminary_receipt_uuid.begin());std::copy_n(b.begin()+32,16,a.final_receipt_uuid.begin());std::copy_n(b.begin()+48,16,a.admission_token_uuid.begin());std::copy_n(b.begin()+64,32,a.demand_sha256.begin());std::copy_n(b.begin()+96,32,a.ordered_profile_sha256.begin());std::copy_n(b.begin()+128,32,a.bound_ast_sha256.begin());std::copy_n(b.begin()+160,32,a.sbxn_sha256.begin());a.catalog_generation=R32(b.data()+192);std::copy_n(b.begin()+216,16,a.mga_snapshot_uuid.begin());return sblr::EncodeSblrLiteralAdmissionV1(&a)==b;
}
bool Sbel(const Bytes&b){return b.size()==176&&std::equal(b.begin(),b.begin()+4,"SBEL")&&R16(b.data()+4)==1&&R16(b.data()+6)==176&&R32(b.data()+8)==176&&!R32(b.data()+12)&&std::any_of(b.begin()+16,b.end(),[](auto x){return x;});}

int main(){
  sblr::SblrLiteralStatementDescriptorProfileV1 p;p.profile_uuid[0]=1;p.statement_receipt_uuid[0]=2;p.catalog_snapshot_uuid[0]=3;p.catalog_generation=4;p.descriptor_uuid[0]=5;p.descriptor_generation=6;p.type_uuid[0]=7;p.codec_id="datatype.int64.le.v1";p.codec_version=1;p.codec_generation=1;p.profile_binding_sha256=sblr::ComputeSblrLiteralDescriptorProfileBindingV1(p,0,0);
  auto sblp=sblr::EncodeSblrLiteralDescriptorProfileV1(p);Fuzz("SBLP",sblp,[&](const Bytes&b){auto d=sblr::DecodeSblrLiteralDescriptorProfileV1(b.data(),b.size());if(d.ok)Require(d.canonical_bytes==b,"SBLP noncanonical acceptance");});
  sblr::SblrLiteralPrebindRequestV1 n;n.preliminary_receipt_uuid[0]=1;n.catalog_snapshot_uuid[0]=2;n.catalog_generation=1;n.mga_snapshot_uuid[0]=3;sblr::SblrLiteralDemandV1 demand;demand.occurrence_id=1;demand.lexical_class=1;demand.context_class=1;demand.lexical_sha256[0]=9;n.demands.push_back(demand);n.demand_sha256=sblr::ComputeSblrLiteralDemandSequenceSha256V1(n.demands);
  auto sbln=sblr::EncodeSblrLiteralPrebindRequestV1(n);Fuzz("SBLN",sbln,[&](const Bytes&b){auto d=sblr::DecodeSblrLiteralPrebindRequestV1(b.data(),b.size());if(d.ok)Require(d.canonical_bytes==b,"SBLN noncanonical acceptance");});
  sblr::SblrLiteralPrebindResultV1 q;q.preliminary_receipt_uuid=n.preliminary_receipt_uuid;q.catalog_snapshot_uuid=n.catalog_snapshot_uuid;q.catalog_generation=1;q.mga_snapshot_uuid=n.mga_snapshot_uuid;q.demand_sha256=n.demand_sha256;q.mappings.push_back({1,sblp});q.ordered_profile_sha256=sblr::ComputeSblrLiteralOrderedProfilesSha256V1(q.mappings);auto sblq=sblr::EncodeSblrLiteralPrebindResultV1(q);Require(Sblq(sblq),"SBLQ seed invalid");Fuzz("SBLQ",sblq,[&](const Bytes&b){(void)Sblq(b);});
  sblr::SblrExpressionNodeTableV1 t;sblr::SblrExpressionLiteralNodeV1 node;node.node_id=7;node.parent_operand_ordinal=1;node.descriptor_uuid=p.descriptor_uuid;node.descriptor_generation=p.descriptor_generation;auto scalar=sblr::EncodeSblrLiteralInt64LeV1(1);node.literal_body.assign(scalar.begin(),scalar.end());t.nodes.push_back(node);auto sbxn=sblr::EncodeSblrExpressionNodeTableV1(t);Fuzz("SBXN",sbxn,[&](const Bytes&b){auto d=sblr::DecodeSblrExpressionNodeTableV1(b.data(),b.size());if(d.ok)Require(d.canonical_bytes==b,"SBXN noncanonical acceptance");});
  auto sbxn_sha=hash::ComputeSha256Digest(sbxn).digest;Bytes ref;U16(&ref,1);U16(&ref,0);U32(&ref,1);U64(&ref,7);ref.insert(ref.end(),sbxn_sha.begin(),sbxn_sha.end());ref.insert(ref.end(),p.descriptor_uuid.begin(),p.descriptor_uuid.end());U64(&ref,p.descriptor_generation);sblr::SblrExpressionNodeReferenceV1 r;Require(sblr::DecodeSblrExpressionNodeReferenceV1(ref.data(),ref.size(),&r),"kind17 seed invalid");Fuzz("kind17",ref,[&](const Bytes&b){sblr::SblrExpressionNodeReferenceV1 x;if(sblr::DecodeSblrExpressionNodeReferenceV1(b.data(),b.size(),&x))Require(b.size()==72,"kind17 extent admitted");});
  sblr::SblrLiteralBoundAstV1 ba;ba.preliminary_receipt_uuid=n.preliminary_receipt_uuid;ba.demand_sha256=n.demand_sha256;auto sbba=sblr::EncodeSblrLiteralBoundAstV1(ba);auto sbba_sha=sblr::ComputeSblrLiteralBoundAstSha256V1(sbba);
  Bytes f;f.insert(f.end(),{'S','B','L','F'});U16(&f,1);U16(&f,208);U32(&f,208+sbba.size()+sbxn.size());U32(&f,0);f.insert(f.end(),n.preliminary_receipt_uuid.begin(),n.preliminary_receipt_uuid.end());f.insert(f.end(),n.demand_sha256.begin(),n.demand_sha256.end());f.insert(f.end(),q.ordered_profile_sha256.begin(),q.ordered_profile_sha256.end());f.insert(f.end(),sbba_sha.begin(),sbba_sha.end());f.insert(f.end(),sbxn_sha.begin(),sbxn_sha.end());U64(&f,1);U64(&f,0);U64(&f,0);f.insert(f.end(),n.mga_snapshot_uuid.begin(),n.mga_snapshot_uuid.end());U32(&f,sbba.size());U32(&f,sbxn.size());f.insert(f.end(),sbba.begin(),sbba.end());f.insert(f.end(),sbxn.begin(),sbxn.end());sblr::SblrLiteralFinalizeRequestV1 fv;Require(sblr::DecodeSblrLiteralFinalizeRequestV1(f.data(),f.size(),&fv),"SBLF seed invalid");Fuzz("SBLF",f,[&](const Bytes&b){sblr::SblrLiteralFinalizeRequestV1 x;(void)sblr::DecodeSblrLiteralFinalizeRequestV1(b.data(),b.size(),&x);});
  sblr::SblrLiteralAdmissionV1 a;a.preliminary_receipt_uuid=n.preliminary_receipt_uuid;a.final_receipt_uuid[0]=4;a.admission_token_uuid[0]=5;a.demand_sha256=n.demand_sha256;a.ordered_profile_sha256=q.ordered_profile_sha256;a.bound_ast_sha256=sbba_sha;a.sbxn_sha256=sbxn_sha;a.catalog_generation=1;a.mga_snapshot_uuid=n.mga_snapshot_uuid;auto sbla=sblr::EncodeSblrLiteralAdmissionV1(&a);Require(Sbla(sbla),"SBLA seed invalid");Fuzz("SBLA",sbla,[&](const Bytes&b){if(Sbla(b))Require(b==sbla,"SBLA altered canonical token");});
  Bytes sbel(176);std::copy_n("SBEL",4,sbel.begin());sbel[4]=1;sbel[6]=176;sbel[8]=176;std::copy_n(sbla.begin()+32,16,sbel.begin()+16);std::copy_n(sbla.begin()+48,16,sbel.begin()+32);std::copy_n(sbla.begin()+232,32,sbel.begin()+48);std::copy(sbba_sha.begin(),sbba_sha.end(),sbel.begin()+80);std::copy(sbxn_sha.begin(),sbxn_sha.end(),sbel.begin()+112);Require(Sbel(sbel),"SBEL seed invalid");Fuzz("SBEL",sbel,[&](const Bytes&b){if(Sbel(b))Require(b.size()==176,"SBEL extent admitted");});
  std::cout<<"seed=0x"<<std::hex<<kSeed<<std::dec<<" iterations="<<kIterations<<" minimized=8 protocols=8\n";return EXIT_SUCCESS;
}
