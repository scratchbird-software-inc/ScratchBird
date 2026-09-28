// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <initializer_list>

namespace s = scratchbird::engine::sblr;
using Id = std::array<std::uint8_t,16>;
using Bytes = std::vector<std::uint8_t>;
static unsigned checks = 0;
void Require(bool ok, const char* reason) {
  ++checks;
  if (!ok) { std::cerr << reason << " check=" << checks << '\n'; std::exit(1); }
}
Id Identity(unsigned suffix) {
  return {1,0xa0,0x7c,0x0a,0x0d,0x5c,0x70,0,0x80,0,0xff,0,0,0,0,
          static_cast<std::uint8_t>(suffix)};
}
template<class T, class Encode>
void Required(const T& source, std::initializer_list<Id T::*> fields, Encode encode) {
  Require(!encode(source).empty(), "positive identity fixture refused");
  for (auto field : fields) {
    for (unsigned offset : {6u,8u}) for (unsigned byte=0;byte<256;++byte) {
      auto value=source; (value.*field)[offset]=byte;
      const bool valid=offset==6 ? byte>>4==7 : byte>>6==2;
      Require(encode(value).empty()!=valid, "required identity version/variant admission");
    }
    auto value=source; (value.*field).fill(0);
    Require(encode(value).empty(), "required nil identity admitted");
  }
}
template<class T, class Encode>
void Optional(T source, Id T::*field, std::uint64_t T::*generation, Encode encode) {
  source.*field={}; source.*generation=0;
  Require(!encode(source).empty(), "absent optional identity refused");
  source.*generation=1;
  Require(encode(source).empty(), "nil identity with generation admitted");
  source.*field=Identity(90);
  Required(source,{field},encode);
  source.*generation=0;
  Require(encode(source).empty(), "present identity without generation admitted");
  for (unsigned version=0;version<16;++version) {
    (source.*field)[6]=version<<4;
    Require(encode(source).empty(), "malformed optional identity mistaken for absence");
  }
  source.*field=Identity(90);
  for (unsigned variant=0;variant<4;++variant) {
    (source.*field)[8]=variant<<6;
    Require(encode(source).empty(), "malformed optional variant mistaken for absence");
  }
}
template<class Decode>
void Wire(const Bytes& canonical, std::initializer_list<unsigned> fields, Decode decode) {
  Require(!canonical.empty() && decode(canonical), "positive wire fixture refused");
  for(auto field:fields) {
    Require(field+16<=canonical.size(), "wire field outside fixture");
    for(unsigned version=0;version<16;++version) if(version!=7) {
      auto bytes=canonical; bytes[field+6]=version<<4;
      Require(!decode(bytes), "wire admitted non-v7 system identity");
    }
    for(unsigned variant=0;variant<4;++variant) if(variant!=2) {
      auto bytes=canonical; bytes[field+8]=variant<<6;
      Require(!decode(bytes), "wire admitted non-RFC system identity");
    }
    auto bytes=canonical;std::fill_n(bytes.begin()+field,16,0);
    Require(!decode(bytes), "wire admitted nil system identity");
  }
}
template<class T, class Encode, class Decode>
void Frame(T fixture, std::initializer_list<unsigned> fields, Encode encode, Decode decode) {
  const auto canonical=encode(fixture);
  Wire(canonical,fields,[&](const Bytes& bytes) {
    auto output=fixture;std::string detail;
    const bool ok=decode(bytes.data(),bytes.size(),&output,&detail);
    Require(encode(output)==(ok?bytes:canonical), "decoder modified refused output or canonical data");
    return ok;
  });
}
int main() {
  using P=s::SblrLiteralStatementDescriptorProfileV1;
  P p;p.profile_uuid=Identity(1);p.statement_receipt_uuid=Identity(2);
  p.catalog_snapshot_uuid=Identity(3);p.catalog_generation=1;
  p.descriptor_uuid=Identity(4);p.descriptor_generation=1;p.type_uuid=Identity(5);
  p.codec_id="datatype.int64.le.v1";p.codec_version=1;p.codec_generation=1;
  p.profile_binding_sha256=s::ComputeSblrLiteralDescriptorProfileBindingV1(p,0,0);
  Required(p,{&P::profile_uuid,&P::statement_receipt_uuid,&P::catalog_snapshot_uuid,
    &P::descriptor_uuid,&P::type_uuid},s::EncodeSblrLiteralDescriptorProfileV1);
  auto profile=s::EncodeSblrLiteralDescriptorProfileV1(p);
  Wire(profile,{16,32,48,72,96},[](const Bytes& b){return s::DecodeSblrLiteralDescriptorProfileV1(b.data(),b.size()).ok;});
  using P2=s::SblrLiteralStatementDescriptorProfileV2;
  P2 p2;p2.profile_uuid=Identity(1);p2.statement_receipt_uuid=Identity(2);
  p2.catalog_snapshot_uuid=Identity(3);p2.catalog_generation=1;
  p2.descriptor_uuid=Identity(4);p2.descriptor_generation=1;p2.type_uuid=Identity(5);
  p2.persisted_descriptor_uuid=Identity(6);p2.persisted_descriptor_generation=1;
  p2.codec_id=p.codec_id;p2.codec_version=1;p2.codec_generation=1;
  p2.profile_binding_sha256=s::ComputeSblrLiteralDescriptorProfileBindingV2(p2,0,0);
  Required(p2,{&P2::profile_uuid,&P2::statement_receipt_uuid,&P2::catalog_snapshot_uuid,
    &P2::descriptor_uuid,&P2::type_uuid,&P2::persisted_descriptor_uuid},s::EncodeSblrLiteralDescriptorProfileV2);
  Wire(s::EncodeSblrLiteralDescriptorProfileV2(p2),{16,32,48,72,96,112},
    [](const Bytes& b){return s::DecodeSblrLiteralDescriptorProfileV2(b.data(),b.size()).ok;});
  using Q=s::SblrLiteralPrebindRequestV1;
  Q q;q.preliminary_receipt_uuid=Identity(2);q.catalog_snapshot_uuid=Identity(3);
  q.catalog_generation=1;q.mga_snapshot_uuid=Identity(7);
  s::SblrLiteralDemandV1 demand;demand.occurrence_id=1;demand.lexical_class=1;demand.context_class=1;demand.lexical_sha256[0]=1;
  q.demands={demand};q.demand_sha256=s::ComputeSblrLiteralDemandSequenceSha256V1(q.demands);
  Required(q,{&Q::preliminary_receipt_uuid,&Q::catalog_snapshot_uuid,&Q::mga_snapshot_uuid},s::EncodeSblrLiteralPrebindRequestV1);
  Wire(s::EncodeSblrLiteralPrebindRequestV1(q),{16,32,72},[](const Bytes& b){return s::DecodeSblrLiteralPrebindRequestV1(b.data(),b.size()).ok;});
  auto bad_demand=q;bad_demand.demands[0].occurrence_id=0;bad_demand.demand_sha256={};
  Require(s::EncodeSblrLiteralPrebindRequestV1(bad_demand).empty(),"failed literal demand digest admitted");
  using G=s::SblrLiteralPrebindResultV1;
  G g;g.preliminary_receipt_uuid=q.preliminary_receipt_uuid;g.catalog_snapshot_uuid=q.catalog_snapshot_uuid;
  g.catalog_generation=1;g.mga_snapshot_uuid=q.mga_snapshot_uuid;g.demand_sha256=q.demand_sha256;
  g.mappings={{1,profile}};g.ordered_profile_sha256=s::ComputeSblrLiteralOrderedProfilesSha256V1(g.mappings);
  Required(g,{&G::preliminary_receipt_uuid,&G::catalog_snapshot_uuid,&G::mga_snapshot_uuid},s::EncodeSblrLiteralPrebindResultV1);
  auto bad_mapping=g;bad_mapping.mappings[0].sblp_bytes[16+6]=0x40;
  bad_mapping.ordered_profile_sha256=s::ComputeSblrLiteralOrderedProfilesSha256V1(bad_mapping.mappings);
  Require(s::EncodeSblrLiteralPrebindResultV1(bad_mapping).empty(),"invalid nested profile or error digest admitted");
  using N=s::SblrLiteralBoundAstNodeV1;
  N node;node.parent_operand_ordinal=1;node.node_id=1;node.descriptor_uuid=Identity(4);
  node.descriptor_generation=1;node.type_uuid=Identity(5);node.profile_uuid=Identity(1);
  node.occurrence_id=1;node.lexical_sha256=demand.lexical_sha256;
  using T=s::SblrLiteralBoundAstV1;
  T t;t.preliminary_receipt_uuid=Identity(2);t.demand_sha256=q.demand_sha256;t.nodes={node};
  Required(t,{&T::preliminary_receipt_uuid},s::EncodeSblrLiteralBoundAstV1);
  Required(node,{&N::descriptor_uuid,&N::type_uuid,&N::profile_uuid},
    [&](N n){auto value=t;value.nodes={n};return s::EncodeSblrLiteralBoundAstV1(value);});
  const auto sbba=s::EncodeSblrLiteralBoundAstV1(t);
  Frame(t,{24,88,112,128},s::EncodeSblrLiteralBoundAstV1,
    [](const auto*b,auto n,auto*out,auto*){return s::DecodeSblrLiteralBoundAstV1(b,n,out);});
  using X=s::SblrExpressionLiteralNodeV1;
  X x;x.node_id=1;x.parent_operand_ordinal=1;x.descriptor_uuid=Identity(4);
  x.descriptor_generation=1;x.literal_body={1,0,0,0,0,0,0,0};
  auto enc_x=[](X n){return s::EncodeSblrExpressionNodeTableV1({{n}});};
  Required(x,{&X::descriptor_uuid},enc_x);
  auto sbxn=enc_x(x);
  Wire(sbxn,{70},[](const Bytes& b){return s::DecodeSblrExpressionNodeTableV1(b.data(),b.size()).ok;});
  for(unsigned version=0;version<16;++version) for(unsigned variant=0;variant<4;++variant) {
    auto n=x;auto id=Identity(30);id[6]=version<<4;id[8]=variant<<6;
    n.literal_body.assign(id.begin(),id.end());auto bytes=enc_x(n);
    auto d=s::DecodeSblrExpressionNodeTableV1(bytes.data(),bytes.size());
    Require(d.ok&&d.table.nodes[0].literal_body==n.literal_body,"opaque literal data restricted to system UUID version");
  }
  for(unsigned byte:{0u,255u}) {
    auto n=x;n.literal_body.assign(16,byte);auto bytes=enc_x(n);
    auto d=s::DecodeSblrExpressionNodeTableV1(bytes.data(),bytes.size());
    Require(d.ok&&d.table.nodes[0].literal_body==n.literal_body,"opaque literal bytes altered");
  }
  auto hash=[](const Bytes& b){return scratchbird::core::hash::ComputeSha256Digest(b).digest;};
  auto put=[](Bytes& b,unsigned at,std::uint64_t n,unsigned size){for(unsigned i=0;i<size;++i)b[at+i]=n>>(8*i);};
  auto id_at=[](Bytes& b,unsigned at,const Id& id){std::copy(id.begin(),id.end(),b.begin()+at);};
  Bytes ref(72);put(ref,0,1,2);put(ref,4,1,4);put(ref,8,1,8);
  auto digest=hash(sbxn);std::copy(digest.begin(),digest.end(),ref.begin()+16);
  id_at(ref,48,Identity(4));put(ref,64,1,8);
  Wire(ref,{48},[](const Bytes& b){s::SblrExpressionNodeReferenceV1 out;out.node_id=777;
    const bool ok=s::DecodeSblrExpressionNodeReferenceV1(b.data(),b.size(),&out);
    Require(ok||out.node_id==777,"rejected literal reference changed destination");return ok;});
  Bytes finalize(208);std::copy_n("SBLF",4,finalize.begin());put(finalize,4,1,2);put(finalize,6,208,2);
  put(finalize,8,208+sbba.size()+sbxn.size(),4);id_at(finalize,16,q.preliminary_receipt_uuid);
  std::copy(q.demand_sha256.begin(),q.demand_sha256.end(),finalize.begin()+32);
  std::copy(g.ordered_profile_sha256.begin(),g.ordered_profile_sha256.end(),finalize.begin()+64);
  auto ast_hash=s::ComputeSblrLiteralBoundAstSha256V1(sbba);
  std::copy(ast_hash.begin(),ast_hash.end(),finalize.begin()+96);
  std::copy(digest.begin(),digest.end(),finalize.begin()+128);put(finalize,160,1,8);
  id_at(finalize,184,q.mga_snapshot_uuid);put(finalize,200,sbba.size(),4);put(finalize,204,sbxn.size(),4);
  finalize.insert(finalize.end(),sbba.begin(),sbba.end());finalize.insert(finalize.end(),sbxn.begin(),sbxn.end());
  Wire(finalize,{16,184},[](const Bytes& b){s::SblrLiteralFinalizeRequestV1 out;out.catalog_generation=777;
    const bool ok=s::DecodeSblrLiteralFinalizeRequestV1(b.data(),b.size(),&out);
    Require(ok||out.catalog_generation==777,"rejected finalization changed destination");return ok;});
  using A=s::SblrLiteralAdmissionV1;
  A a;a.preliminary_receipt_uuid=Identity(2);a.final_receipt_uuid=Identity(8);
  a.admission_token_uuid=Identity(9);a.catalog_generation=1;a.mga_snapshot_uuid=Identity(7);
  a.demand_sha256=q.demand_sha256;a.ordered_profile_sha256=g.ordered_profile_sha256;
  a.bound_ast_sha256=ast_hash;a.sbxn_sha256=digest;
  Required(a,{&A::preliminary_receipt_uuid,&A::final_receipt_uuid,&A::admission_token_uuid,&A::mga_snapshot_uuid},
    [](A value){return s::EncodeSblrLiteralAdmissionV1(&value);});
  using E=s::SblrLiteralExecutorEvidenceV1;
  E evidence;evidence.descriptor_uuid=Identity(4);evidence.descriptor_generation=1;evidence.canonical_value_sha256=hash(x.literal_body);
  Required(evidence,{&E::descriptor_uuid},[](E value){auto h=s::ComputeSblrLiteralExecutorEvidenceSha256V1(value);return h?Bytes(h->begin(),h->end()):Bytes{};});
  std::cout<<"literal UUID admission checks="<<checks<<'\n';
}
